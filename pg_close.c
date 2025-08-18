#include "pg_handle.h"

#include <stdlib.h>

int pg_close(PGHandle* h) {
	if (!h) return 0;
	if (h->qp_left) ibv_destroy_qp(h->qp_left);
	if (h->qp_right) ibv_destroy_qp(h->qp_right);
	if (h->mr_send_left) ibv_dereg_mr(h->mr_send_left);
	if (h->mr_recv_left) ibv_dereg_mr(h->mr_recv_left);
	if (h->mr_send_right) ibv_dereg_mr(h->mr_send_right);
	if (h->mr_recv_right) ibv_dereg_mr(h->mr_recv_right);
	if (h->sendbuf_left) free(h->sendbuf_left);
	if (h->recvbuf_left) free(h->recvbuf_left);
	if (h->sendbuf_right) free(h->sendbuf_right);
	if (h->recvbuf_right) free(h->recvbuf_right);
	if (h->cq) ibv_destroy_cq(h->cq);
	if (h->pd) ibv_dealloc_pd(h->pd);
	if (h->context) ibv_close_device(h->context);
	return 0;
}

