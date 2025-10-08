#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdlib.h>
#include <stdio.h>

int pg_close(PGHandle* pg_handle) {
    if (!pg_handle) {
        return -1;
    }

    // Close all RDMA connections associated with the handle
    if (pg_handle->conn) {
        rdma_disconnect(pg_handle->conn);
        rdma_destroy_qp(pg_handle->conn);
        rdma_destroy_id(pg_handle->conn);
        pg_handle->conn = NULL;
    }

    // Free any allocated resources
    if (pg_handle->mr) {
        ibv_dereg_mr(pg_handle->mr);
        pg_handle->mr = NULL;
    }

    if (pg_handle->pd) {
        ibv_dealloc_pd(pg_handle->pd);
        pg_handle->pd = NULL;
    }

    if (pg_handle->ctx) {
        ibv_close_device(pg_handle->ctx);
        pg_handle->ctx = NULL;
    }

    free(pg_handle);

    return 0;
}