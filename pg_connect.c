#include "pg_connect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

static int tcp_connect(const char* host, int port) {
	struct addrinfo hints, *res = NULL, *rp = NULL;
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo(host, portstr, &hints, &res);
	if (rc != 0) {
		if (DEBUG) fprintf(stderr, "[tcp_connect] getaddrinfo(%s:%d) failed: %s\n", host, port, gai_strerror(rc));
		return -1;
	}
	int sock = -1;
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		sock = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0) continue;
		if (connect(sock, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) break;
		close(sock);
		sock = -1;
	}
	freeaddrinfo(res);
	return sock;
}

static int tcp_listen_any(int port) {
	int sock = (int)socket(AF_INET6, SOCK_STREAM, 0);
	if (sock < 0) return -1;
	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons((uint16_t)port);
	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		if (DEBUG) fprintf(stderr, "[tcp_listen_any] bind port %d failed: %s\n", port, strerror(errno));
		close(sock);
		return -1;
	}
	if (listen(sock, 8) != 0) {
		if (DEBUG) fprintf(stderr, "[tcp_listen_any] listen port %d failed: %s\n", port, strerror(errno));
		close(sock);
		return -1;
	}
	return sock;
}

static int tcp_accept(int lsock) {
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	int csock = (int)accept(lsock, (struct sockaddr*)&ss, &slen);
	return csock;
}

typedef struct ExchangeMsg {
	uint32_t qp_num;
	uint16_t lid;
	uint8_t gid[16];
	uint32_t rkey;
	uint64_t buf_addr;
	uint8_t has_gid;
} ExchangeMsg;

static void fill_gid(struct ibv_context* ctx, uint8_t port, uint8_t gid[16], int* has_gid) {
	struct ibv_port_attr pattr;
	if (ibv_query_port(ctx, port, &pattr) != 0) {
		memset(gid, 0, 16);
		*has_gid = 0;
		return;
	}
	union ibv_gid tmpgid;
	if (ibv_query_gid(ctx, port, 0, &tmpgid) == 0) {
		memcpy(gid, &tmpgid, 16);
		*has_gid = 1;
	} else {
		memset(gid, 0, 16);
		*has_gid = 0;
	}
}

static int create_qp(PGHandle* h, struct ibv_qp** out_qp) {
	struct ibv_qp_init_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.recv_cq = h->cq;
	attr.send_cq = h->cq;
	attr.cap.max_send_wr = PG_DEFAULT_QP_DEPTH;
	attr.cap.max_recv_wr = PG_DEFAULT_QP_DEPTH;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	struct ibv_qp* qp = ibv_create_qp(h->pd, &attr);
	if (!qp) {
		if (DEBUG) fprintf(stderr, "[R%d] ibv_create_qp failed\n", h->my_rank);
		return -1;
	}
	*out_qp = qp;
	return 0;
}

static int qp_to_init(struct ibv_qp* qp, uint8_t port) {
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
	int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	int rc = ibv_modify_qp(qp, &attr, mask);
	return rc;
}

static int qp_to_rtr(struct ibv_qp* qp, const PgRemoteInfo* rmt, uint8_t port, enum ibv_mtu mtu) {
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.dest_qp_num = rmt->qp_num;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = rmt->has_gid;
	attr.ah_attr.dlid = rmt->lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = port;
	if (rmt->has_gid) {
		memcpy(&attr.ah_attr.grh.dgid, rmt->gid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.sgid_index = 0;
		attr.ah_attr.grh.traffic_class = 0;
	}
	int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
			IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	int rc = ibv_modify_qp(qp, &attr, mask);
	return rc;
}

static int qp_to_rts(struct ibv_qp* qp) {
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 1;
	int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	int rc = ibv_modify_qp(qp, &attr, mask);
	return rc;
}

static int query_lid(struct ibv_context* ctx, uint8_t port, uint16_t* lid_out) {
	struct ibv_port_attr pattr;
	if (ibv_query_port(ctx, port, &pattr) != 0) return -1;
	*lid_out = pattr.lid;
	return 0;
}

static int pick_first_device(PGHandle* h) {
	int num = 0;
	struct ibv_device** devs = ibv_get_device_list(&num);
	if (!devs || num == 0) {
		if (DEBUG) fprintf(stderr, "[R%d] No RDMA devices found\n", h->my_rank);
		return -1;
	}
	h->context = ibv_open_device(devs[0]);
	ibv_free_device_list(devs);
	if (!h->context) {
		if (DEBUG) fprintf(stderr, "[R%d] ibv_open_device failed\n", h->my_rank);
		return -1;
	}
	return 0;
}

static int alloc_basic_resources(PGHandle* h) {
	h->pd = ibv_alloc_pd(h->context);
	if (!h->pd) {
		if (DEBUG) fprintf(stderr, "[R%d] ibv_alloc_pd failed\n", h->my_rank);
		return -1;
	}
	h->cq = ibv_create_cq(h->context, PG_DEFAULT_CQ_DEPTH, NULL, NULL, 0);
	if (!h->cq) {
		if (DEBUG) fprintf(stderr, "[R%d] ibv_create_cq failed\n", h->my_rank);
		return -1;
	}
	return 0;
}

static int register_side_buffers(PGHandle* h, int right_side) {
	void **sendbuf = right_side ? &h->sendbuf_right : &h->sendbuf_left;
	void **recvbuf = right_side ? &h->recvbuf_right : &h->recvbuf_left;
	struct ibv_mr **ms = right_side ? &h->mr_send_right : &h->mr_send_left;
	struct ibv_mr **mr = right_side ? &h->mr_recv_right : &h->mr_recv_left;
	size_t alloc_size = (h->max_message_bytes + 4095) & ~((size_t)4095);
	if (posix_memalign(sendbuf, 4096, alloc_size) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] posix_memalign(sendbuf) failed\n", h->my_rank);
		return -1;
	}
	if (posix_memalign(recvbuf, 4096, alloc_size) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] posix_memalign(recvbuf) failed\n", h->my_rank);
		return -1;
	}
	*ms = ibv_reg_mr(h->pd, *sendbuf, alloc_size, IBV_ACCESS_LOCAL_WRITE);
	*mr = ibv_reg_mr(h->pd, *recvbuf, alloc_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!*ms || !*mr) {
		if (DEBUG) fprintf(stderr, "[R%d] ibv_reg_mr failed (send_mr=%p recv_mr=%p)\n", h->my_rank, (void*)*ms, (void*)*mr);
		return -1;
	}
	return 0;
}

static int exchange_info(int lsock, int csock, struct ibv_qp* my_qp, PGHandle* h, PgRemoteInfo* out_remote, int right_side) {
	uint16_t my_lid = 0;
	query_lid(h->context, h->ib_port, &my_lid);
	ExchangeMsg msg;
	memset(&msg, 0, sizeof(msg));
	msg.qp_num = my_qp->qp_num;
	msg.lid = my_lid;
	if (right_side) {
		msg.rkey = h->mr_recv_right ? (uint32_t)h->mr_recv_right->rkey : 0;
		msg.buf_addr = (uint64_t)(uintptr_t)h->recvbuf_right;
	} else {
		msg.rkey = h->mr_recv_left ? (uint32_t)h->mr_recv_left->rkey : 0;
		msg.buf_addr = (uint64_t)(uintptr_t)h->recvbuf_left;
	}
	fill_gid(h->context, h->ib_port, msg.gid, (int*)&msg.has_gid);
	// Send then recv
	if (send(csock, &msg, sizeof(msg), 0) != sizeof(msg)) {
		if (DEBUG) fprintf(stderr, "[R%d] exchange_info send failed: %s\n", h->my_rank, strerror(errno));
		return -1;
	}
	ExchangeMsg peer;
	if (recv(csock, &peer, sizeof(peer), MSG_WAITALL) != sizeof(peer)) {
		if (DEBUG) fprintf(stderr, "[R%d] exchange_info recv failed: %s\n", h->my_rank, strerror(errno));
		return -1;
	}
	memset(out_remote, 0, sizeof(*out_remote));
	out_remote->qp_num = peer.qp_num;
	out_remote->lid = peer.lid;
	out_remote->rkey = peer.rkey;
	out_remote->buf_addr = peer.buf_addr;
	out_remote->has_gid = peer.has_gid;
	memcpy(out_remote->gid, peer.gid, 16);
	return 0;
}

int pg_connect_ring(char** serverlist, int len, int idx, PGHandle* h) {
	if (!serverlist || len <= 1 || idx < 0 || idx > len || !h){
		if (DEBUG) fprintf(stderr, "[R%d] Invalid parameters for pg_connect_ring\n", idx);
		// print the specific parameters
		fprintf(stderr, "[R%d] serverlist=%p len=%d idx=%d h=%p\n", idx, (void*)serverlist, len, idx, (void*)h);
		return -1;
	}
	memset(h, 0, sizeof(*h));
	h->world_size = len;
	h->my_rank = idx;
	h->left_rank = (idx - 1 + len) % len;
	h->right_rank = (idx + 1) % len;
	h->ib_port = PG_DEFAULT_IB_PORT;
	h->max_message_bytes = PG_MAX_MESSAGE_BYTES;

	if (pick_first_device(h) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] pick_first_device failed\n", idx);
		return -1;
	}
	if (alloc_basic_resources(h) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] alloc_basic_resources failed\n", idx);
		return -1;
	}
	if (register_side_buffers(h, 0) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] register_side_buffers(left) failed\n", idx);
		return -1;
	}
	if (register_side_buffers(h, 1) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] register_side_buffers(right) failed\n", idx);
		return -1;
	}

	if (create_qp(h, &h->qp_left) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] create_qp(left) failed\n", idx);
		return -1;
	}
	if (create_qp(h, &h->qp_right) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] create_qp(right) failed\n", idx);
		return -1;
	}
	if (qp_to_init(h->qp_left, h->ib_port) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_init(left) failed\n", idx);
		return -1;
	}
	if (qp_to_init(h->qp_right, h->ib_port) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_init(right) failed\n", idx);
		return -1;
	}

	int my_port = PG_DEFAULT_TCP_PORT + idx; // unique port per rank for simplicity
	int lsock = tcp_listen_any(my_port);
	if (lsock < 0) {
		if (DEBUG) fprintf(stderr, "[R%d] tcp_listen_any(%d) failed\n", idx, my_port);
		return -1;
	}

	// Connect to right neighbor, receive from left neighbor.
	int right_port = PG_DEFAULT_TCP_PORT + h->right_rank;
	int csock_right = -1;
	if (idx == 0) {
		// Rank 0: actively connect, possibly retry until neighbor is up
		for (int attempts = 0; attempts < 200; ++attempts) {
			csock_right = tcp_connect(serverlist[h->right_rank], right_port);
			if (csock_right >= 0) break;
			if (DEBUG && (attempts % 20 == 0)) fprintf(stderr, "[R%d] attempt %d: connect to %s:%d failed\n", idx, attempts, serverlist[h->right_rank], right_port);
			usleep(100 * 1000);
		}
		if (DEBUG) fprintf(stderr, "[R%d] connect to right %d: %s\n", idx, h->right_rank, csock_right >= 0 ? "ok" : "fail");
		if (csock_right < 0) return -1;
	}

	int csock_left = -1;
	// Accept from left
	csock_left = tcp_accept(lsock);
	if (csock_left < 0) {
		if (DEBUG) fprintf(stderr, "[R%d] accept from left failed: %s\n", idx, strerror(errno));
		return -1;
	}

	if (idx != 0) {
		// Others connect to right after accepting left to avoid deadlock
		for (int attempts = 0; attempts < 200; ++attempts) {
			csock_right = tcp_connect(serverlist[h->right_rank], right_port);
			if (csock_right >= 0) break;
			if (DEBUG && (attempts % 20 == 0)) fprintf(stderr, "[R%d] attempt %d: connect to %s:%d failed\n", idx, attempts, serverlist[h->right_rank], right_port);
			usleep(100 * 1000);
		}
		if (DEBUG) fprintf(stderr, "[R%d] connect to right %d: %s\n", idx, h->right_rank, csock_right >= 0 ? "ok" : "fail");
		if (csock_right < 0) return -1;
	}

	// Exchange QP info with both neighbors
	if (exchange_info(lsock, csock_right, h->qp_right, h, &h->remote_right, 1) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] exchange_info(right) failed\n", idx);
		return -1;
	}
	if (exchange_info(lsock, csock_left, h->qp_left, h, &h->remote_left, 0) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] exchange_info(left) failed\n", idx);
		return -1;
	}

	close(csock_right);
	close(csock_left);
	close(lsock);

	// Move QPs to RTR/RTS
	if (qp_to_rtr(h->qp_right, &h->remote_right, h->ib_port, PG_DEFAULT_MTU) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_rtr(right) failed\n", idx);
		return -1;
	}
	if (qp_to_rtr(h->qp_left, &h->remote_left, h->ib_port, PG_DEFAULT_MTU) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_rtr(left) failed\n", idx);
		return -1;
	}
	if (qp_to_rts(h->qp_right) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_rts(right) failed\n", idx);
		return -1;
	}
	if (qp_to_rts(h->qp_left) != 0) {
		if (DEBUG) fprintf(stderr, "[R%d] qp_to_rts(left) failed\n", idx);
		return -1;
	}

	if (DEBUG) fprintf(stderr, "[R%d] Connected: left=%d right=%d\n", idx, h->left_rank, h->right_rank);
	return 0;
}

