#include "pg_collectives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

size_t datatype_sizeof(DATATYPE dtype) {
	switch (dtype) {
		case INT: return sizeof(int);
		case DOUBLE: return sizeof(double);
		default: return 0;
	}
}

void apply_op(void* dst, const void* src, int count, DATATYPE dtype, OPERATION op) {
	if (dtype == INT) {
		int* d = (int*)dst;
		const int* s = (const int*)src;
		if (op == SUM) {
			for (int i = 0; i < count; ++i) d[i] += s[i];
		} else {
			for (int i = 0; i < count; ++i) d[i] *= s[i];
		}
	} else if (dtype == DOUBLE) {
		double* d = (double*)dst;
		const double* s = (const double*)src;
		if (op == SUM) {
			for (int i = 0; i < count; ++i) d[i] += s[i];
		} else {
			for (int i = 0; i < count; ++i) d[i] *= s[i];
		}
	}
}

static int post_send(struct ibv_qp* qp, struct ibv_mr* mr, void* buf, size_t len, uint32_t imm) {
	struct ibv_sge sge;
	sge.addr = (uintptr_t)buf;
	sge.length = (uint32_t)len;
	sge.lkey = mr->lkey;
	struct ibv_send_wr wr;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = 1;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = imm ? IBV_WR_SEND_WITH_IMM : IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	if (imm) wr.imm_data = htonl(imm);
	struct ibv_send_wr* bad = NULL;
	return ibv_post_send(qp, &wr, &bad);
}

static int post_recv(struct ibv_qp* qp, struct ibv_mr* mr, void* buf, size_t len) {
	struct ibv_sge sge;
	sge.addr = (uintptr_t)buf;
	sge.length = (uint32_t)len;
	sge.lkey = mr->lkey;
	struct ibv_recv_wr wr;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = 2;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	struct ibv_recv_wr* bad = NULL;
	return ibv_post_recv(qp, &wr, &bad);
}

static int wait_one(struct ibv_cq* cq) {
	struct ibv_wc wc;
	while (1) {
		int n = ibv_poll_cq(cq, 1, &wc);
		if (n < 0) return -1;
		if (n == 0) continue;
		if (wc.status != IBV_WC_SUCCESS) return -1;
		return 0;
	}
}

int pg_reduce_scatter(void* sendbuf, void* recvbuf, int count, DATATYPE dtype, OPERATION op, PGHandle* h) {
	if (!h || count <= 0) return -1;
	size_t elem = datatype_sizeof(dtype);
	size_t bytes = (size_t)count * elem;
	int n = h->world_size;
	int rank = h->my_rank;

	// Initialize local partial from sendbuf
	memcpy(recvbuf, sendbuf, bytes);

	// Ring reduce-scatter: n-1 steps
	for (int step = 0; step < n - 1; ++step) {
		// Post receive from left
		if (post_recv(h->qp_left, h->mr_recv_left, h->recvbuf_left, bytes) != 0) return -1;
		// Copy my current partial into right send buffer
		memcpy(h->sendbuf_right, recvbuf, bytes);
		if (post_send(h->qp_right, h->mr_send_right, h->sendbuf_right, bytes, 0) != 0) return -1;
		// Wait for both send and recv
		if (wait_one(h->cq) != 0) return -1; // send
		if (wait_one(h->cq) != 0) return -1; // recv
		// Reduce into my buffer
		apply_op(recvbuf, h->recvbuf_left, count, dtype, op);
		if (DEBUG) fprintf(stderr, "[R%d] RS step %d done\n", rank, step);
	}
	return 0;
}

int pg_all_gather(void* sendbuf, void* recvbuf, int count, DATATYPE dtype, PGHandle* h) {
	if (!h || count <= 0) return -1;
	size_t elem = datatype_sizeof(dtype);
	size_t bytes = (size_t)count * elem;
	int n = h->world_size;
	int rank = h->my_rank;

	// For simplicity, assume recvbuf already contains my segment (from reduce_scatter)
	// Ring all-gather: n-1 steps circulating chunks
	for (int step = 0; step < n - 1; ++step) {
		if (post_recv(h->qp_left, h->mr_recv_left, h->recvbuf_left, bytes) != 0) return -1;
		memcpy(h->sendbuf_right, recvbuf, bytes);
		if (post_send(h->qp_right, h->mr_send_right, h->sendbuf_right, bytes, 0) != 0) return -1;
		if (wait_one(h->cq) != 0) return -1;
		if (wait_one(h->cq) != 0) return -1;
		// Integrate received chunk by overwriting (gather)
		memcpy(recvbuf, h->recvbuf_left, bytes);
		if (DEBUG) fprintf(stderr, "[R%d] AG step %d done\n", rank, step);
	}
	return 0;
}

int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE dtype, OPERATION op, PGHandle* h) {
	if (pg_reduce_scatter(sendbuf, recvbuf, count, dtype, op, h) != 0) return -1;
	if (pg_all_gather(recvbuf, recvbuf, count, dtype, h) != 0) return -1;
	return 0;
}

