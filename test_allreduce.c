#include "pg_connect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple send/receive test after connect_process_group()
// Each rank writes a message into its right neighbor's buffer
// and then reads back from its left neighbor's buffer.

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <comma_separated_server_list> <rank>\n", argv[0]);
        return 1;
    }

    char *serverlist = argv[1];
    int rank = atoi(argv[2]);

    void *pg_handle_void = NULL;

    printf("Rank %d: Connecting to process group...\n", rank);
    if (connect_process_group(serverlist, &pg_handle_void) != 0) {
        fprintf(stderr, "Rank %d: connect_process_group failed\n", rank);
        return 1;
    }
    pg_handle_t *pg_handle = (pg_handle_t *)pg_handle_void;

    printf("Rank %d: Connected. size=%d\n", rank, pg_handle->size);

    // Get neighbors
    int left  = (rank - 1 + pg_handle->size) % pg_handle->size;
    int right = (rank + 1) % pg_handle->size;

    // Prepare a message
    char message[64];
    snprintf(message, sizeof(message), "Hello from rank %d", rank);

    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr = NULL;

    memset(&wr, 0, sizeof(wr));
    sge.addr   = (uintptr_t)message;
    sge.length = strlen(message) + 1;
    sge.lkey   = pg_handle->mr_send->lkey;

    wr.wr_id      = rank;  // track with rank ID
    wr.next       = NULL;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = pg_handle->remote_addrs[right];
    wr.wr.rdma.rkey        = pg_handle->remote_rkeys[right];

    // Post send
    if (ibv_post_send(pg_handle->qps[1], &wr, &bad_wr)) {
        fprintf(stderr, "Rank %d: ibv_post_send failed\n", rank);
        pg_close(pg_handle);
        return 1;
    }

    // Poll for completion
    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(pg_handle->cq, 1, &wc);
    } while (ne == 0);

    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Rank %d: send completion failed (status=%d)\n", rank, wc.status);
        pg_close(pg_handle);
        return 1;
    }

    // Small delay to let neighbors write
    sleep(1);

    // Print what we received in our recvbuf (left neighbor should have written here)
    printf("Rank %d: Received buffer = \"%s\"\n", rank, (char *)pg_handle->recvbuf);

    // Cleanup
    pg_close(pg_handle);

    return 0;
}
