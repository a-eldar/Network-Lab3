#define _POSIX_C_SOURCE 200112L
#include "pg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <inttypes.h>

/* Simple CHECK macro */
#define CHECK_RET(x, msg) do { if ((x) != 0) { fprintf(stderr, "%s: %s\n", msg, strerror(errno)); } } while(0)

/* encode wr_id values so we can see send/recv and peer */
#define WRID_SEND(peer)  ( (uint64_t)(peer) << 32 | 0x1 )
#define WRID_RECV(peer)  ( (uint64_t)(peer) << 32 | 0x2 )

typedef struct peer_info {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
    union { uint8_t gid[16]; } gid;
    uint64_t vaddr;
    uint32_t rkey;
} peer_info_t;

struct pg_conn {
    int nprocs;
    int myrank;
    char **hosts;

    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp **qps;
    peer_info_t *remote_info;
    struct ibv_mr *mr_send;
    struct ibv_mr *mr_recv;
    void *send_buf;
    void *recv_buf;
    size_t buf_bytes;
    int sock_listen;
};

/* TCP helpers */
static int tcp_listen(int port) {
    int s, opt = 1;
    struct sockaddr_in addr;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    if (listen(s, 8) < 0) { close(s); return -1; }
    return s;
}

static int tcp_connect_to_host(const char *host, int port) {
    struct addrinfo hints, *res, *rp;
    int s = -1;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(s); s = -1;
    }
    freeaddrinfo(res);
    return s;
}

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, p+sent, len-sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, p+got, len-got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* ibverbs helpers */
static struct ibv_qp* create_qp(struct ibv_pd *pd, struct ibv_cq *cq) {
    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = cq;
    attr.recv_cq = cq;
    attr.cap.max_send_wr = 32;
    attr.cap.max_recv_wr = 32;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.qp_type = IBV_QPT_RC;
    return ibv_create_qp(pd, &attr);
}

static int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    return ibv_modify_qp(qp, &attr, flags);
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t dest_qp_num, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = dest_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;
    int flags = IBV_QP_STATE | IBV_QP_AV |
                IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                IBV_QP_MIN_RNR_TIMER;
    return ibv_modify_qp(qp, &attr, flags);
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT |
                IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    return ibv_modify_qp(qp, &attr, flags);
}

/* parse host list */
static char **parse_host_list(const char *s, int *out_count) {
    char *copy = strdup(s);
    char *tok;
    int cap = 8, n = 0;
    char **arr = malloc(cap * sizeof(char*));
    tok = strtok(copy, " \t");
    while (tok) {
        if (n == cap) {
            cap *= 2;
            arr = realloc(arr, cap*sizeof(char*));
        }
        arr[n++] = strdup(tok);
        tok = strtok(NULL, " \t");
    }
    free(copy);
    *out_count = n;
    return arr;
}

/* debug helpers */
static const char *wc_status_str(int status) {
    switch (status) {
        case IBV_WC_SUCCESS: return "IBV_WC_SUCCESS";
        case IBV_WC_LOC_LEN_ERR: return "IBV_WC_LOC_LEN_ERR";
        case IBV_WC_LOC_QP_OP_ERR: return "IBV_WC_LOC_QP_OP_ERR";
        case IBV_WC_LOC_EEC_OP_ERR: return "IBV_WC_LOC_EEC_OP_ERR";
        case IBV_WC_LOC_PROT_ERR: return "IBV_WC_LOC_PROT_ERR";
        case IBV_WC_WR_FLUSH_ERR: return "IBV_WC_WR_FLUSH_ERR";
        case IBV_WC_MW_BIND_ERR: return "IBV_WC_MW_BIND_ERR";
        case IBV_WC_BAD_RESP_ERR: return "IBV_WC_BAD_RESP_ERR";
        case IBV_WC_LOC_ACCESS_ERR: return "IBV_WC_LOC_ACCESS_ERR";
        case IBV_WC_REM_INV_REQ_ERR: return "IBV_WC_REM_INV_REQ_ERR";
        case IBV_WC_REM_ACCESS_ERR: return "IBV_WC_REM_ACCESS_ERR";
        case IBV_WC_REM_OP_ERR: return "IBV_WC_REM_OP_ERR";
        case IBV_WC_RETRY_EXC_ERR: return "IBV_WC_RETRY_EXC_ERR";
        case IBV_WC_RNR_RETRY_EXC_ERR: return "IBV_WC_RNR_RETRY_EXC_ERR";
        // case IBV_WC_TIMEOUT_ERR: return "IBV_WC_TIMEOUT_ERR";
        case IBV_WC_RESP_TIMEOUT_ERR: return "IBV_WC_RESP_TIMEOUT_ERR";
        case IBV_WC_GENERAL_ERR: return "IBV_WC_GENERAL_ERR";
        default: return "IBV_WC_UNKNOWN";
    }
}

static int debug_poll_cq(struct ibv_cq *cq, int want) {
    int got = 0;
    while (got < want) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "ibv_poll_cq error: %d\n", ne);
            return -1;
        }
        if (ne == 0) continue;
        got += 1;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "WC ERROR: status=%d (%s), opcode=%d, wr_id=0x%" PRIx64 ", vendor_err=%u, qp_num=%u\n",
                    wc.status, wc_status_str(wc.status), wc.opcode, (uint64_t)wc.wr_id, wc.vendor_err, wc.qp_num);
            return -1;
        }
    }
    return 0;
}

/* posting helpers */
static int post_recv(pg_conn_t *pg, int peer, void *buf, size_t len) {
    struct ibv_sge sge;
    struct ibv_recv_wr rr, *bad;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = pg->mr_recv->lkey;
    memset(&rr, 0, sizeof(rr));
    rr.wr_id = WRID_RECV(peer);
    rr.next = NULL;
    rr.sg_list = &sge;
    rr.num_sge = 1;
    int rc = ibv_post_recv(pg->qps[peer], &rr, &bad);
    if (rc) {
        fprintf(stderr, "ibv_post_recv(peer=%d) failed: %d\n", peer, rc);
    }
    return rc;
}

static int post_send(pg_conn_t *pg, int peer, void *buf, size_t len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = pg->mr_send->lkey;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = WRID_SEND(peer);
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    int rc = ibv_post_send(pg->qps[peer], &wr, &bad);
    if (rc) {
        fprintf(stderr, "ibv_post_send(peer=%d) failed: %d\n", peer, rc);
    }
    return rc;
}

/* simple int32 reduction */
static void reduce_int32_inplace(int32_t *dst, int32_t *src, int count) {
    for (int i = 0; i < count; ++i) dst[i] += src[i];
}

/* connect */
int connect_process_group(const char *host_list, int myrank, void **pg_handle_out) {
    int i, ret = -1;
    int nproc;
    char **hosts = parse_host_list(host_list, &nproc);
    if (nproc < 2) { fprintf(stderr, "need >=2 hosts\n"); return -1; }
    if (myrank < 0 || myrank >= nproc) { fprintf(stderr, "invalid rank\n"); return -1; }

    pg_conn_t *pg = calloc(1, sizeof(*pg));
    pg->nprocs = nproc;
    pg->myrank = myrank;
    pg->hosts = hosts;

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) { perror("ibv_get_device_list"); goto out; }
    pg->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!pg->ctx) { perror("ibv_open_device"); goto out; }
    pg->pd = ibv_alloc_pd(pg->ctx);
    if (!pg->pd) { perror("ibv_alloc_pd"); goto out; }
    pg->cq = ibv_create_cq(pg->ctx, 512, NULL, NULL, 0);
    if (!pg->cq) { perror("ibv_create_cq"); goto out; }

    pg->qps = calloc(nproc, sizeof(struct ibv_qp*));
    for (i = 0; i < nproc; ++i) {
        pg->qps[i] = create_qp(pg->pd, pg->cq);
        if (!pg->qps[i]) { fprintf(stderr, "create_qp failed for %d\n", i); goto out; }
        if (modify_qp_to_init(pg->qps[i]) != 0) { fprintf(stderr, "modify to INIT failed for qp %d\n", i); goto out; }
    }

    /* allocate buffers */
    size_t default_count = 4096; /* elements */
    pg->buf_bytes = default_count * sizeof(int32_t);
    void *p1 = NULL, *p2 = NULL;
    if (posix_memalign(&p1, 4096, pg->buf_bytes) != 0) p1 = NULL;
    if (posix_memalign(&p2, 4096, pg->buf_bytes) != 0) p2 = NULL;
    if (!p1 || !p2) { fprintf(stderr, "posix_memalign failed\n"); goto out; }
    pg->send_buf = p1; pg->recv_buf = p2;
    memset(pg->send_buf, 0, pg->buf_bytes);
    memset(pg->recv_buf, 0, pg->buf_bytes);
    pg->mr_send = ibv_reg_mr(pg->pd, pg->send_buf, pg->buf_bytes, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    pg->mr_recv = ibv_reg_mr(pg->pd, pg->recv_buf, pg->buf_bytes, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!pg->mr_send || !pg->mr_recv) { fprintf(stderr, "reg_mr failed\n"); goto out; }

    /* local info */
    peer_info_t local;
    memset(&local, 0, sizeof(local));
    struct ibv_port_attr port_attr;
    if (ibv_query_port(pg->ctx, 1, &port_attr)) { perror("ibv_query_port"); goto out; }
    local.lid = port_attr.lid;
    local.qpn = pg->qps[pg->myrank]->qp_num;
    local.psn = 0;
    local.vaddr = (uintptr_t)pg->mr_send->addr;
    local.rkey = pg->mr_send->rkey;

    int listen_sock = tcp_listen(PG_PORT_BASE + pg->myrank);
    if (listen_sock < 0) { perror("tcp_listen"); goto out; }
    pg->sock_listen = listen_sock;

    pg->remote_info = calloc(nproc, sizeof(peer_info_t));

    for (i = 0; i < nproc; ++i) {
        if (i == pg->myrank) {
            pg->remote_info[i] = local;
            continue;
        }
        if (pg->myrank < i) {
            int s = -1; int tries = 0;
            while ((s = tcp_connect_to_host(hosts[i], PG_PORT_BASE + i)) < 0 && tries < 60) { tries++; sleep(1); }
            if (s < 0) { fprintf(stderr, "connect to %s failed\n", hosts[i]); goto out; }
            if (send_all(s, &local, sizeof(local)) != 0) { close(s); goto out; }
            if (recv_all(s, &pg->remote_info[i], sizeof(peer_info_t)) != 0) { close(s); goto out; }
            close(s);
        } else {
            int c = accept(listen_sock, NULL, NULL);
            if (c < 0) { perror("accept"); goto out; }
            if (recv_all(c, &pg->remote_info[i], sizeof(peer_info_t)) != 0) { close(c); goto out; }
            if (send_all(c, &local, sizeof(local)) != 0) { close(c); goto out; }
            close(c);
        }
    }

    /* Transition QPs to RTR/RTS and show info */
    for (i = 0; i < nproc; ++i) {
        if (i == pg->myrank) continue;
        fprintf(stderr, "peer %d: local_qp=%u remote_qpn=%u remote_lid=%u\n",
                i, pg->qps[i]->qp_num, pg->remote_info[i].qpn, pg->remote_info[i].lid);
        if (modify_qp_to_rtr(pg->qps[i], pg->remote_info[i].qpn, pg->remote_info[i].lid, pg->remote_info[i].gid.gid) != 0) {
            fprintf(stderr, "modify to RTR failed for peer %d\n", i); goto out;
        }
        if (modify_qp_to_rts(pg->qps[i]) != 0) {
            fprintf(stderr, "modify to RTS failed for peer %d\n", i); goto out;
        }
    }

    *pg_handle_out = pg;
    ret = 0;
out:
    if (ret != 0) {
        if (pg) {
            if (pg->cq) ibv_destroy_cq(pg->cq);
            if (pg->pd) ibv_dealloc_pd(pg->pd);
            if (pg->ctx) ibv_close_device(pg->ctx);
            free(pg);
        }
    }
    return ret;
}

/* all-reduce (int32 only for simplicity). Conservative, debug-friendly. */
int pg_all_reduce(void *sendbuf_v, void *recvbuf_v, int count, int datatype_bytes, int myrank, void *pg_handle) {
    if (datatype_bytes != 4) {
        fprintf(stderr, "only 4-byte int32 supported\n");
        return -1;
    }
    pg_conn_t *pg = (pg_conn_t*)pg_handle;
    if (!pg) return -1;
    int n = pg->nprocs;
    int rank = myrank;
    int elems_per_chunk = (count + n - 1) / n;
    size_t chunk_bytes = elems_per_chunk * datatype_bytes;

    struct ibv_port_attr port_attr;
    if (ibv_query_port(pg->ctx, 1, &port_attr) == 0) {
        fprintf(stderr, "port active_mtu=%d\n", port_attr.active_mtu);
    }

    if (chunk_bytes > pg->buf_bytes) {
        fprintf(stderr, "chunk_bytes (%zu) > buffer (%zu). Increase buffer size\n", chunk_bytes, pg->buf_bytes);
        return -1;
    }

    /* copy input into local send_buf */
    memcpy(pg->send_buf, sendbuf_v, count * datatype_bytes);
    memset(pg->recv_buf, 0, pg->buf_bytes);

    int next = (rank + 1) % n;
    int prev = (rank - 1 + n) % n;

    /* Pre-post (n-1) recvs in rotating slots to avoid deadlock */
    for (int s = 0; s < n-1; ++s) {
        void *recv_ptr = (char*)pg->recv_buf + (s % (n-1)) * chunk_bytes;
        if (post_recv(pg, prev, recv_ptr, chunk_bytes) != 0) {
            fprintf(stderr, "post_recv failed pre-post\n");
            return -1;
        }
    }

    /* Initialize accumulator for own chunk (placed at recv_buf offset rank*chunk_bytes) */
    int32_t *acc = (int32_t*)((char*)pg->recv_buf + rank * chunk_bytes);
    int this_chunk_elems = count - rank * elems_per_chunk;
    if (this_chunk_elems > elems_per_chunk) this_chunk_elems = elems_per_chunk;
    if (this_chunk_elems < 0) this_chunk_elems = 0;
    memset(acc, 0, elems_per_chunk * sizeof(int32_t));
    if (this_chunk_elems > 0) memcpy(acc, (char*)pg->send_buf + rank*elems_per_chunk*datatype_bytes, this_chunk_elems*datatype_bytes);

    /* Reduce-scatter: n-1 steps */
    for (int s = 0; s < n-1; ++s) {
        int send_chunk_idx = (rank - s + n) % n;
        void *send_ptr = (char*)pg->send_buf + send_chunk_idx * chunk_bytes;

        if (post_send(pg, next, send_ptr, chunk_bytes) != 0) {
            fprintf(stderr, "post_send failed step %d\n", s); return -1;
        }
        if (debug_poll_cq(pg->cq, 1) != 0) { fprintf(stderr, "send completion failed\n"); return -1; }

        /* wait for recv completion and reduce */
        if (debug_poll_cq(pg->cq, 1) != 0) { fprintf(stderr, "recv completion failed\n"); return -1; }
        void *just_recv = (char*)pg->recv_buf + (s % (n-1)) * chunk_bytes;
        int dst_idx = (rank - s - 1 + n) % n;
        int32_t *dst = (int32_t*)((char*)pg->recv_buf + dst_idx * chunk_bytes);
        reduce_int32_inplace(dst, (int32_t*)just_recv, elems_per_chunk);

        /* repost the recv in the same slot for next steps if needed (we posted n-1 initially, so it's fine) */
    }

    /* Allgather: n-1 steps */
    for (int s = 0; s < n-1; ++s) {
        int send_idx = (rank - s + n) % n;
        void *send_ptr = (char*)pg->recv_buf + send_idx * chunk_bytes;
        void *recv_ptr = (char*)pg->recv_buf + ((rank - s - 1 + n) % n) * chunk_bytes;
        if (post_recv(pg, prev, recv_ptr, chunk_bytes) != 0) { fprintf(stderr, "post_recv allgather failed\n"); return -1; }
        if (post_send(pg, next, send_ptr, chunk_bytes) != 0) { fprintf(stderr, "post_send allgather failed\n"); return -1; }
        if (debug_poll_cq(pg->cq, 1) != 0) { fprintf(stderr, "send completion allgather failed\n"); return -1; }
        if (debug_poll_cq(pg->cq, 1) != 0) { fprintf(stderr, "recv completion allgather failed\n"); return -1; }
    }

    memcpy(recvbuf_v, pg->recv_buf, count * datatype_bytes);
    return 0;
}

int pg_close(void *pg_handle) {
    pg_conn_t *pg = (pg_conn_t*)pg_handle;
    if (!pg) return 0;
    if (pg->mr_send) ibv_dereg_mr(pg->mr_send);
    if (pg->mr_recv) ibv_dereg_mr(pg->mr_recv);
    if (pg->send_buf) free(pg->send_buf);
    if (pg->recv_buf) free(pg->recv_buf);
    if (pg->qps) {
        for (int i = 0; i < pg->nprocs; ++i) {
            if (pg->qps[i]) ibv_destroy_qp(pg->qps[i]);
        }
        free(pg->qps);
    }
    if (pg->cq) ibv_destroy_cq(pg->cq);
    if (pg->pd) ibv_dealloc_pd(pg->pd);
    if (pg->ctx) ibv_close_device(pg->ctx);
    if (pg->hosts) {
        for (int i = 0; i < pg->nprocs; ++i) free(pg->hosts[i]);
        free(pg->hosts);
    }
    if (pg->remote_info) free(pg->remote_info);
    if (pg->sock_listen) close(pg->sock_listen);
    free(pg);
    return 0;
}
