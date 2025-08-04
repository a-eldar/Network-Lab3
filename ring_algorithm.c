// ring_algorithm.c
#include "rdma_allreduce.h"

int perform_ring_reduce_scatter(void *data, int count, DATATYPE datatype, OPERATION op, PGHandle *pg_handle) {
    if (!data || count <= 0 || !pg_handle || !pg_handle->ring_initialized) {
        return -1;
    }
    
    size_t element_size = get_datatype_size(datatype);
    size_t total_size = count * element_size;
    size_t chunk_size = total_size / pg_handle->num_processes;
    size_t remainder = total_size % pg_handle->num_processes;
    
    // Copy input data to working buffer
    memcpy(pg_handle->work_buffer, data, total_size);
    
    // Perform reduce-scatter phase
    for (int step = 0; step < pg_handle->num_processes - 1; step++) {
        // Calculate which chunk this process will send and expect to receive
        int send_chunk = (pg_handle->my_rank - step + pg_handle->num_processes) % pg_handle->num_processes;
        int recv_chunk = (pg_handle->my_rank - step - 1 + pg_handle->num_processes) % pg_handle->num_processes;
        
        size_t send_offset = send_chunk * chunk_size;
        size_t recv_offset = recv_chunk * chunk_size;
        
        // Calculate chunk sizes (last chunk may be larger due to remainder)
        size_t send_size = chunk_size;
        size_t recv_size = chunk_size;
        if (send_chunk == pg_handle->num_processes - 1) {
            send_size += remainder;
        }
        if (recv_chunk == pg_handle->num_processes - 1) {
            recv_size += remainder;
        }
        
        // Copy data to send buffer
        memcpy(pg_handle->right_conn.send_buf, 
               (char*)pg_handle->work_buffer + send_offset, send_size);
        
        // Send data to right neighbor using RDMA write
        uint64_t remote_recv_offset = pg_handle->right_conn.remote_addr + recv_offset;
        
        struct ibv_sge sge = {
            .addr = (uint64_t)pg_handle->right_conn.send_buf,
            .length = send_size,
            .lkey = pg_handle->right_conn.send_mr->lkey
        };
        
        struct ibv_send_wr wr = {
            .wr_id = step,
            .next = NULL,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma.remote_addr = remote_recv_offset,
            .wr.rdma.rkey = pg_handle->right_conn.remote_rkey
        };
        
        struct ibv_send_wr *bad_wr;
        if (ibv_post_send(pg_handle->right_conn.qp, &wr, &bad_wr) != 0) {
            fprintf(stderr, "Failed to post RDMA write in reduce-scatter step %d\n", step);
            return -1;
        }
        
        // Wait for completion
        struct ibv_wc wc;
        int ne;
        do {
            ne = ibv_poll_cq(pg_handle->right_conn.cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "Failed to poll CQ in reduce-scatter step %d\n", step);
                return -1;
            }
        } while (ne == 0);
        
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "RDMA write failed in reduce-scatter step %d: %s\n", 
                    step, ibv_wc_status_str(wc.status));
            return -1;
        }
        
        // Simple synchronization: sleep to ensure data arrives
        // In a production implementation, you might use proper synchronization
        usleep(1000); // 1ms
        
        // Apply reduction operation on received data
        if (step < pg_handle->num_processes - 2) {
            void *local_chunk = (char*)pg_handle->work_buffer + recv_offset;
            void *received_chunk = (char*)pg_handle->left_conn.recv_buf + recv_offset;
            int elements_in_chunk = recv_size / element_size;
            
            apply_operation(local_chunk, received_chunk, elements_in_chunk, datatype, op);
        }
    }
    
    return 0;
}

int perform_ring_allgather(void *data, int count, DATATYPE datatype, PGHandle *pg_handle) {
    if (!data || count <= 0 || !pg_handle || !pg_handle->ring_initialized) {
        return -1;
    }
    
    size_t element_size = get_datatype_size(datatype);
    size_t total_size = count * element_size;
    size_t chunk_size = total_size / pg_handle->num_processes;
    size_t remainder = total_size % pg_handle->num_processes;
    
    // Perform all-gather phase
    for (int step = 0; step < pg_handle->num_processes - 1; step++) {
        // Calculate which chunk to send and where to receive
        int send_chunk = (pg_handle->my_rank + 1 + step) % pg_handle->num_processes;
        int recv_chunk = (pg_handle->my_rank + step) % pg_handle->num_processes;
        
        size_t send_offset = send_chunk * chunk_size;
        size_t recv_offset = recv_chunk * chunk_size;
        
        // Calculate chunk sizes
        size_t send_size = chunk_size;
        if (send_chunk == pg_handle->num_processes - 1) {
            send_size += remainder;
        }
        
        // Copy chunk to send buffer
        memcpy(pg_handle->right_conn.send_buf,
               (char*)pg_handle->work_buffer + send_offset, send_size);
        
        // Send reduced chunk to right neighbor
        uint64_t remote_recv_offset = pg_handle->right_conn.remote_addr + recv_offset;
        
        struct ibv_sge sge = {
            .addr = (uint64_t)pg_handle->right_conn.send_buf,
            .length = send_size,
            .lkey = pg_handle->right_conn.send_mr->lkey
        };
        
        struct ibv_send_wr wr = {
            .wr_id = step + pg_handle->num_processes,
            .next = NULL,
            .sg_list = &sge,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma.remote_addr = remote_recv_offset,
            .wr.rdma.rkey = pg_handle->right_conn.remote_rkey
        };
        
        struct ibv_send_wr *bad_wr;
        if (ibv_post_send(pg_handle->right_conn.qp, &wr, &bad_wr) != 0) {
            fprintf(stderr, "Failed to post RDMA write in all-gather step %d\n", step);
            return -1;
        }
        
        // Wait for completion
        struct ibv_wc wc;
        int ne;
        do {
            ne = ibv_poll_cq(pg_handle->right_conn.cq, 1, &wc);
            if (ne < 0) {
                fprintf(stderr, "Failed to poll CQ in all-gather step %d\n", step);
                return -1;
            }
        } while (ne == 0);
        
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "RDMA write failed in all-gather step %d: %s\n", 
                    step, ibv_wc_status_str(wc.status));
            return -1;
        }
        
        // Simple synchronization
        usleep(1000); // 1ms
        
        // Copy received chunk to working buffer
        size_t recv_size = chunk_size;
        if (recv_chunk == pg_handle->num_processes - 1) {
            recv_size += remainder;
        }
        
        memcpy((char*)pg_handle->work_buffer + recv_offset,
               (char*)pg_handle->left_conn.recv_buf + recv_offset, recv_size);
    }
    
    // Copy final result back to output buffer
    memcpy(data, pg_handle->work_buffer, total_size);
    
    return 0;
}