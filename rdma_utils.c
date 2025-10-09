#include "rdma_utils.h"


int rdma_write_to_right(PGHandle *pg_handle, size_t actual_size) {   
    // Get neighbors (ring topology)
    int rank = pg_handle->rank;
    int right_neighbor = (rank + 1) % pg_handle->num_servers;

    // Send message to right neighbor using RDMA Write
    // We write to the right neighbor's receive buffer
    struct ibv_sge sge = {
        .addr = (uintptr_t)pg_handle->sendbuf,
        .length = actual_size,
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

int poll_for_completion(PGHandle *pg_handle) {
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

// Ring barrier synchronization
// Uses the last 4 bytes of the recvbuf as a sync flag location
int ring_barrier(PGHandle *pg_handle) {
    int rank = pg_handle->rank;
    int right_neighbor = (rank + 1) % pg_handle->num_servers;
    
    // Sync flag is stored at the end of the receive buffer
    size_t sync_offset = pg_handle->bufsize - sizeof(int);
    volatile int *local_sync_ptr = (volatile int *)((char *)pg_handle->recvbuf + sync_offset);
    
    printf("Rank %d: Entering barrier\n", rank);
    
    // Step 1: Set our local sync flag to 1 (so we can write it to neighbor)
    int sync_flag = 1;
    int *sync_source = (int *)((char *)pg_handle->sendbuf + sync_offset);
    *sync_source = sync_flag;

    if (rank != 0) {
        // Spin on our local recvbuf sync location
        int timeout = 0;
        const int MAX_TIMEOUT = 100000000;  // 100 million iterations
        
        while (*local_sync_ptr != 1) {
            timeout++;
            if (timeout > MAX_TIMEOUT) {
                fprintf(stderr, "Rank %d: Barrier timeout - left neighbor didn't signal (flag=%d)\n", 
                        rank, *local_sync_ptr);
                return 1;
            }
        }
    }
    
    // Step 2: Write sync flag to right neighbor's recvbuf at sync_offset
    struct ibv_sge sge = {
        .addr = (uintptr_t)sync_source,
        .length = sizeof(int),
        .lkey = pg_handle->mr_send->lkey
    };
    
    struct ibv_send_wr wr = {
        .wr_id = rank + 1000,  // Different wr_id to distinguish from data transfers
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = pg_handle->remote_addrs[right_neighbor] + sync_offset,
            .rkey = pg_handle->remote_rkeys[right_neighbor]
        },
        .next = NULL
    };
    
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(pg_handle->qps[1], &wr, &bad_wr) != 0) {
        fprintf(stderr, "Rank %d: Failed to post barrier sync write\n", rank);
        return 1;
    }
    
    // Step 3: Wait for our write completion
    if (poll_for_completion(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: Barrier write completion failed\n", rank);
        return 1;
    }
    
    // Step 4: Wait for left neighbor to write to our buffer
    if (rank == 0) {
        // Spin on our local recvbuf sync location
        int timeout = 0;
        const int MAX_TIMEOUT = 100000000;  // 100 million iterations
        
        while (*local_sync_ptr != 1) {
            timeout++;
            if (timeout > MAX_TIMEOUT) {
                fprintf(stderr, "Rank %d: Barrier timeout - left neighbor didn't signal (flag=%d)\n", 
                        rank, *local_sync_ptr);
                return 1;
            }
        }
    }
    
    printf("Rank %d: Barrier complete (received signal from left)\n", rank);
    
    // Step 5: Reset the flag for next barrier
    *local_sync_ptr = 0;
    
    return 0;
}