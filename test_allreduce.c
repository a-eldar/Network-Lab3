#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pg_allreduce.h"

static void usage(const char* prog) {
	fprintf(stderr, "Usage: %s -myindex <idx> -list <host1,host2,...>\n", prog);
}

int main(int argc, char** argv) {
	int myindex = -1;
	char* list = NULL;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-myindex") == 0 && i + 1 < argc) myindex = atoi(argv[++i]);
		else if (strcmp(argv[i], "-list") == 0 && i + 1 < argc) list = argv[++i];
	}
	if (myindex < 0 || !list) {
		usage(argv[0]);
		return 1;
	}

	// Parse list
	int count = 1;
	for (const char* c = list; *c; ++c) if (*c == ',') ++count;
	char** servers = (char**)calloc((size_t)count, sizeof(char*));
	char* tmp = strdup(list);
	int idx = 0;
	char* saveptr = NULL;
	for (char* tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
		servers[idx++] = strdup(tok);
	}

	PGHandle h;
	if (connect_process_group(servers, count, myindex, &h) != 0) {
		fprintf(stderr, "Failed to connect process group\n");
		return 2;
	}

	// Connectivity test: send a small message to right neighbor and receive from left.
	const char* msg = "hello";
	size_t msglen = strlen(msg) + 1;
	memcpy(h.sendbuf_right, msg, msglen);
	// Post recv on left and send on right
	struct ibv_sge sge_send = { .addr = (uintptr_t)h.sendbuf_right, .length = (uint32_t)msglen, .lkey = h.mr_send_right->lkey };
	struct ibv_send_wr wr_send; memset(&wr_send, 0, sizeof(wr_send));
	wr_send.sg_list = &sge_send; wr_send.num_sge = 1; wr_send.opcode = IBV_WR_SEND; wr_send.send_flags = IBV_SEND_SIGNALED;
	struct ibv_send_wr* bad_send = NULL;

	struct ibv_sge sge_recv = { .addr = (uintptr_t)h.recvbuf_left, .length = (uint32_t)msglen, .lkey = h.mr_recv_left->lkey };
	struct ibv_recv_wr wr_recv; memset(&wr_recv, 0, sizeof(wr_recv));
	wr_recv.sg_list = &sge_recv; wr_recv.num_sge = 1;
	struct ibv_recv_wr* bad_recv = NULL;

	if (ibv_post_recv(h.qp_left, &wr_recv, &bad_recv) != 0) { fprintf(stderr, "post recv failed\n"); return 3; }
	if (ibv_post_send(h.qp_right, &wr_send, &bad_send) != 0) { fprintf(stderr, "post send failed\n"); return 3; }
	struct ibv_wc wc;
	int got = 0;
	while (got < 2) {
		int n = ibv_poll_cq(h.cq, 1, &wc);
		if (n > 0) {
			if (wc.status != IBV_WC_SUCCESS) { fprintf(stderr, "wc status %d\n", wc.status); return 3; }
			++got;
		}
	}
	if (strcmp((char*)h.recvbuf_left, msg) == 0) {
		fprintf(stderr, "[R%d] Connectivity test ok: '%s'\n", h.my_rank, (char*)h.recvbuf_left);
	} else {
		fprintf(stderr, "[R%d] Connectivity test mismatch: '%s'\n", h.my_rank, (char*)h.recvbuf_left);
	}

	// Small all-reduce test with ints
	int val = h.my_rank + 1;
	int out = 0;
	if (pg_all_reduce(&val, &out, 1, INT, SUM, &h) != 0) {
		fprintf(stderr, "all_reduce failed\n");
		return 4;
	}
	int expected = (count * (count + 1)) / 2; // sum 1..N
	fprintf(stderr, "[R%d] all_reduce result=%d expected=%d\n", h.my_rank, out, expected);

	pg_close(&h);
	for (int i = 0; i < count; ++i) free(servers[i]);
	free(servers);
	free(tmp);
	return 0;
}

