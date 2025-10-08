#include "rdma_utils.h"


int rdma_write_to_right(pg_handle_t *pg_handle, char* message, size_t msg_len) {   
    if(msg_len > RDMA_BUFFER_SIZE) {
        fprintf(stderr, "Message length exceeds buffer size\n");
        return 1;
    }
    // Get neighbors (ring topology)
    int rank = pg_handle->rank;
    int right_neighbor = (rank + 1) % pg_handle->size;
    // Prepare a message in our send buffer
    memcpy(pg_handle->sendbuf, message, strlen(message) + 1);
    

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
        fprintf(stderr, "Rank %d: Failed to post RDMA write\n", rank);
        return 1;
    }
    
    return 0;
}

int poll_for_completion(pg_handle_t *pg_handle) {
    int rank = pg_handle->rank;
    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(pg_handle->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Rank %d: Failed to poll CQ\n", rank);
            return 1;
        }
    } while (ne == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Rank %d: Work completion failed with status %s\n", 
                rank, ibv_wc_status_str(wc.status));
        return 1;
    }
    
    printf("Rank %d: RDMA write completed successfully\n", rank);
    return 0;
}