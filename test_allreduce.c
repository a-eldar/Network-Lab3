#include "pg_connect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple send/receive test after connect_process_group()
// Each rank writes a message into its right neighbor's buffer
// and then reads back from its left neighbor's buffer.
// Note: Each rank runs this program with a different rank number.

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
    
    // Cast handle to the correct type
    pg_handle_t *pg_handle = (pg_handle_t *)pg_handle_void;
    
    printf("Rank %d: Connected! Size=%d\n", pg_handle->rank, pg_handle->size);

    // Get neighbors (ring topology)
    int right_neighbor = (rank + 1) % pg_handle->size;
    int left_neighbor = (rank - 1 + pg_handle->size) % pg_handle->size;
    
    printf("Rank %d: Left neighbor = %d, Right neighbor = %d\n", 
           rank, left_neighbor, right_neighbor);

    // Prepare a message in our send buffer
    char message[256];
    snprintf(message, sizeof(message), "Hello from rank %d!", rank);
    memcpy(pg_handle->sendbuf, message, strlen(message) + 1);
    
    printf("Rank %d: Prepared message: \"%s\"\n", rank, message);

    // Send message to right neighbor using RDMA Write
    // We write to the right neighbor's receive buffer
    struct ibv_sge sge = {
        .addr = (uintptr_t)pg_handle->sendbuf,
        .length = strlen(message) + 1,
        .lkey = pg_handle->mr_send->lkey
    };
    
    struct ibv_send_wr wr = {
        .wr_id = rank,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = pg_handle->remote_addrs[right_neighbor],
            .rkey = pg_handle->remote_rkeys[right_neighbor]
        },
        .next = NULL
    };
    
    struct ibv_send_wr *bad_wr;
    
    printf("Rank %d: Posting RDMA write to rank %d...\n", rank, right_neighbor);
    if (ibv_post_send(pg_handle->qps[1], &wr, &bad_wr) != 0) {
        fprintf(stderr, "Rank %d: Failed to post send\n", rank);
        pg_close(pg_handle_void);
        return 1;
    }
    
    // Wait for completion
    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(pg_handle->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Rank %d: Failed to poll CQ\n", rank);
            pg_close(pg_handle_void);
            return 1;
        }
    } while (ne == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Rank %d: Work completion failed with status %s\n", 
                rank, ibv_wc_status_str(wc.status));
        pg_close(pg_handle_void);
        return 1;
    }
    
    printf("Rank %d: RDMA write completed successfully\n", rank);

    // Small delay to let neighbors write
    sleep(1);

    // Print what we received in our recvbuf (left neighbor should have written here)
    printf("Rank %d: Received buffer = \"%s\"\n", rank, (char *)pg_handle->recvbuf);

    // Cleanup
    pg_close(pg_handle_void);

    return 0;
}
