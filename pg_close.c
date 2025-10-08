#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdlib.h>
#include <stdio.h>

int pg_close(PGHandle* pg_handle) {
    if (!pg_handle) {
        return -1;
    }

    // 1. Clean up Queue Pairs
    if (pg_handle->qps) {
        // Destroy QPs for both left and right neighbors
        for (int i = 0; i < 2; i++) {
            if (pg_handle->qps[i]) {
                if (ibv_destroy_qp(pg_handle->qps[i])) {
                    fprintf(stderr, "Failed to destroy QP %d\n", i);
                }
            }
        }
        free(pg_handle->qps);
    }

    // 2. Clean up Completion Queue
    if (pg_handle->cq) {
        if (ibv_destroy_cq(pg_handle->cq)) {
            fprintf(stderr, "Failed to destroy CQ\n");
        }
    }

    // 3. Clean up Memory Regions
    if (pg_handle->mr_send) {
        if (ibv_dereg_mr(pg_handle->mr_send)) {
            fprintf(stderr, "Failed to deregister send MR\n");
        }
    }

    if (pg_handle->mr_recv) {
        if (ibv_dereg_mr(pg_handle->mr_recv)) {
            fprintf(stderr, "Failed to deregister recv MR\n");
        }
    }

    // 4. Clean up Protection Domain
    if (pg_handle->pd) {
        if (ibv_dealloc_pd(pg_handle->pd)) {
            fprintf(stderr, "Failed to deallocate PD\n");
        }
    }

    // 5. Close RDMA device context
    if (pg_handle->ctx) {
        if (ibv_close_device(pg_handle->ctx)) {
            fprintf(stderr, "Failed to close RDMA device\n");
        }
    }

    // 6. Free allocated buffers
    if (pg_handle->sendbuf) {
        free(pg_handle->sendbuf);
    }
    
    if (pg_handle->recvbuf) {
        free(pg_handle->recvbuf);
    }

    // 7. Free remote info arrays
    if (pg_handle->remote_rkeys) {
        free(pg_handle->remote_rkeys);
    }

    if (pg_handle->remote_addrs) {
        free(pg_handle->remote_addrs);
    }

    // 8. Free server names
    if (pg_handle->servernames) {
        for (int i = 0; i < pg_handle->num_servers; i++) {
            if (pg_handle->servernames[i]) {
                free(pg_handle->servernames[i]);
            }
        }
        free(pg_handle->servernames);
    }

    // 9. Finally, free the handle itself
    free(pg_handle);

    return 0;
}