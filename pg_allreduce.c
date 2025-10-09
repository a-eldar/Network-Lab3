#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>  // For gettimeofday


static size_t get_datatype_size(DATATYPE datatype) {
    switch (datatype) {
        case INT:
            return sizeof(int);
        case DOUBLE:
            return sizeof(double);
        default:
            return 0;
    }
}

static void perform_operation(void *dst, const void *src, int count, DATATYPE datatype, OPERATION op) {
    int i;
    if (datatype == INT) {
        int *d = (int *)dst;
        const int *s = (const int *)src;
        
        if (op == SUM) {
            for (i = 0; i < count; i++) {
                d[i] += s[i];
            }
        } else if (op == MULT) {
            for (i = 0; i < count; i++) {
                d[i] *= s[i];
            }
        }
    } else if (datatype == DOUBLE) {
        double *d = (double *)dst;
        const double *s = (const double *)src;
        
        if (op == SUM) {
            for (i = 0; i < count; i++) {
                d[i] += s[i];
            }
        } else if (op == MULT) {
            for (i = 0; i < count; i++) {
                d[i] *= s[i];
            }
        }
    }
}


// Rendezvous method: Local write + remote read
static int transfer_data_rendezvous(PGHandle *pg_handle, size_t actual_size) {
    if(ring_barrier(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: BARRIER ring_barrier failed\n", pg_handle->rank);
        return 1;
    }

    if(rdma_write_to_right(pg_handle, actual_size) != 0) {
        fprintf(stderr, "Rank %d: rdma_write_to_right failed\n", pg_handle->rank);
        return 1;
    }
    // Wait for completion
    if(poll_for_completion(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: poll_for_completion failed\n", pg_handle->rank);
        return 1;
    }

    if(ring_barrier(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: BARRIER ring_barrier failed\n", pg_handle->rank);
        return 1;
    }
    
    return 0;
}



int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle) {
    if (!sendbuf || !recvbuf || count <= 0 || !pg_handle ) {
        fprintf(stderr, "Invalid parameters for all_reduce\n");
        return -1;
    }

    void *rdma_recvbuf = pg_handle->recvbuf;
    void *rdma_sendbuf = pg_handle->sendbuf;
    
    size_t dtype_size = get_datatype_size(datatype);
    if (dtype_size == 0) {
        fprintf(stderr, "Invalid datatype\n");
        return -1;
    }
    
    size_t total_size = count * dtype_size;
    int n = pg_handle->num_servers;
    int idx = pg_handle->rank;
    
    // Calculate chunk size for each server
    int chunk_size = count / n;
    int remainder = count % n;
    
    // Copy input to output buffer initially
    memcpy(recvbuf, sendbuf, total_size);
    void *temp_buf = malloc(total_size);
    if (!temp_buf) {
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }
    
    // Phase 1: Reduce-scatter using ring algorithm
    // Each server will accumulate values for its designated chunk
    for (int step = 0; step < n - 1; step++) {
        memset(rdma_sendbuf, 0, pg_handle->bufsize);
        memset(rdma_recvbuf, 0, pg_handle->bufsize);
        // Calculate which chunk to send/receive
        int send_chunk_id = (idx - step + n) % n;
        int recv_chunk_id = (idx - step - 1 + n) % n;
        
        // Calculate offsets and sizes
        size_t send_offset = send_chunk_id * chunk_size * dtype_size;
        size_t recv_offset = recv_chunk_id * chunk_size * dtype_size;
        
        int send_count = chunk_size;
        int recv_count = chunk_size;
        
        // Handle remainder elements for last chunk
        if (send_chunk_id == n - 1) {
            send_count = chunk_size + remainder;
        }
        if (recv_chunk_id == n - 1) {
            recv_count = chunk_size + remainder;
        }
        
        size_t send_bytes = send_count * dtype_size;
        size_t recv_bytes = recv_count * dtype_size;
        
        // Copy data to send buffer
        memcpy(rdma_sendbuf, (char *)recvbuf + send_offset, send_bytes);

        
        // Transfer data using selected method (rendezvous or eager)
        transfer_data_rendezvous(pg_handle, send_bytes);

        

        memcpy(temp_buf, rdma_recvbuf, recv_bytes);
        
        // Perform reduction operation
        perform_operation((char *)recvbuf + recv_offset,
                         temp_buf,
                         recv_count,
                         datatype,
                         op);
    }

    // Phase 2: All-gather using ring algorithm
    // Each server broadcasts its chunk to all others
    for (int step = 0; step < n - 1; step++) {
        // Calculate which chunk to send/receive
        int send_chunk_id = (idx - step + n + 1) % n;
        int recv_chunk_id = (idx - step + n) % n;
        
        // Calculate offsets and sizes
        size_t send_offset = send_chunk_id * chunk_size * dtype_size;
        size_t recv_offset = recv_chunk_id * chunk_size * dtype_size;
        
        int send_count = chunk_size;
        int recv_count = chunk_size;
        
        // Handle remainder elements for last chunk
        if (send_chunk_id == n - 1) {
            send_count = chunk_size + remainder;
        }
        if (recv_chunk_id == n - 1) {
            recv_count = chunk_size + remainder;
        }
        
        size_t send_bytes = send_count * dtype_size;
        size_t recv_bytes = recv_count * dtype_size;
        
        // Copy data to send buffer
        memcpy(rdma_sendbuf, (char *)recvbuf + send_offset, send_bytes);
        
        // Transfer data using selected method (rendezvous or eager)
        transfer_data_rendezvous(pg_handle, send_bytes);
        
        // Copy received chunk to result buffer
        memcpy((char *)recvbuf + recv_offset, rdma_recvbuf, recv_bytes);
        
    }
    
    return 0;
}