#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Define transfer method - easy to swap between RENDEZVOUS and EAGER
#define USE_RENDEZVOUS_METHOD 1  // Set to 0 for EAGER method (using RDMA Write)

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

static int synchronize_servers(PGHandle *pg_handle) {
    // Simple barrier synchronization
    int sync_val = 1;
    size_t sync_offset = pg_handle->work_buffer_size - sizeof(int);
    
    // Write sync value locally
    memcpy((char *)pg_handle->left_conn->buf + sync_offset, &sync_val, sizeof(int));
    
    // Wait for left neighbor to set their sync flag by reading from them
    int received_sync = 0;
    int attempts = 0;
    void *local_sync_buf = malloc(sizeof(int));
    
    while (received_sync == 0 && attempts < 1000) {
        // Read sync flag from left neighbor's buffer
        if (post_rdma_read(pg_handle->right_conn,
                          local_sync_buf,
                          sizeof(int),
                          pg_handle->left_conn->remote_addr + sync_offset,
                          pg_handle->left_conn->remote_rkey) < 0) {
            free(local_sync_buf);
            return -1;
        }
        
        if (wait_for_completion(pg_handle->right_conn) < 0) {
            free(local_sync_buf);
            return -1;
        }
        
        memcpy(&received_sync, local_sync_buf, sizeof(int));
        if (received_sync == 0) {
            usleep(1000); // Wait 1ms
        }
        attempts++;
    }
    
    // Clear sync flag for next use
    sync_val = 0;
    memcpy((char *)pg_handle->left_conn->buf + sync_offset, &sync_val, sizeof(int));
    
    free(local_sync_buf);
    return 0;
}

// Rendezvous method: Local write + remote read
static int transfer_data_rendezvous(PGHandle *pg_handle, void *send_data, void *recv_data, size_t size) {
    // Write data locally for the left neighbor to read
    memcpy(pg_handle->left_conn->buf, send_data, size);
    
    // Synchronize to ensure data is written
    if (synchronize_servers(pg_handle) < 0) {
        return -1;
    }
    
    // Read data from left neighbor (server on our left writes, we read from them)
    if (post_rdma_read(pg_handle->right_conn,
                      recv_data,
                      size,
                      pg_handle->left_conn->remote_addr,
                      pg_handle->left_conn->remote_rkey) < 0) {
        return -1;
    }
    
    if (wait_for_completion(pg_handle->right_conn) < 0) {
        return -1;
    }
    
    return 0;
}

// Eager method: RDMA Write (for easy swapping)
static int transfer_data_eager(PGHandle *pg_handle, void *send_data, void *recv_data, size_t size) {
    // Write data to right neighbor's buffer
    if (post_rdma_write(pg_handle->right_conn,
                       send_data,
                       size,
                       pg_handle->right_conn->remote_addr,
                       pg_handle->right_conn->remote_rkey) < 0) {
        return -1;
    }
    
    if (wait_for_completion(pg_handle->right_conn) < 0) {
        return -1;
    }
    
    // Synchronize to ensure write is complete
    if (synchronize_servers(pg_handle) < 0) {
        return -1;
    }
    
    // Read from local buffer where left neighbor wrote
    memcpy(recv_data, pg_handle->left_conn->buf, size);
    
    return 0;
}

// Wrapper function to easily switch between methods
static inline int transfer_data(PGHandle *pg_handle, void *send_data, void *recv_data, size_t size) {
#if USE_RENDEZVOUS_METHOD
    return transfer_data_rendezvous(pg_handle, send_data, recv_data, size);
#else
    return transfer_data_eager(pg_handle, send_data, recv_data, size);
#endif
}

int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle) {
    if (!sendbuf || !recvbuf || count <= 0 || !pg_handle || !pg_handle->connected) {
        fprintf(stderr, "Invalid parameters for all_reduce\n");
        return -1;
    }
    
    size_t dtype_size = get_datatype_size(datatype);
    if (dtype_size == 0) {
        fprintf(stderr, "Invalid datatype\n");
        return -1;
    }
    
    size_t total_size = count * dtype_size;
    int n = pg_handle->num_servers;
    int idx = pg_handle->server_idx;
    
    // Calculate chunk size for each server
    int chunk_size = count / n;
    int remainder = count % n;
    
    // Copy input to output buffer initially
    memcpy(recvbuf, sendbuf, total_size);
    
    // Allocate temporary buffers for ring operations
    void *send_chunk = malloc(total_size);
    void *recv_chunk = malloc(total_size);
    
    if (!send_chunk || !recv_chunk) {
        fprintf(stderr, "Failed to allocate temporary buffers\n");
        return -1;
    }
    
    // Phase 1: Reduce-scatter using ring algorithm
    // Each server will accumulate values for its designated chunk
    for (int step = 0; step < n - 1; step++) {
        // Calculate which chunk to send/receive
        int send_chunk_id = (idx - step + n) % n;
        int recv_chunk_id = (idx - step - 1 + n) % n;
        
        // Calculate offsets and sizes
        int send_offset = send_chunk_id * chunk_size;
        int recv_offset = recv_chunk_id * chunk_size;
        
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
        memcpy(send_chunk, (char *)recvbuf + send_offset * dtype_size, send_bytes);
        
        // Transfer data using selected method (rendezvous or eager)
        if (transfer_data(pg_handle, send_chunk, recv_chunk, 
                         (send_bytes > recv_bytes) ? send_bytes : recv_bytes) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Perform reduction operation
        perform_operation((char *)recvbuf + recv_offset * dtype_size,
                         recv_chunk,
                         recv_count,
                         datatype,
                         op);
        
        // Synchronize before next step
        if (synchronize_servers(pg_handle) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
    }
    
    // Phase 2: All-gather using ring algorithm
    // Each server broadcasts its chunk to all others
    for (int step = 0; step < n - 1; step++) {
        // Calculate which chunk to send/receive
        int send_chunk_id = (idx - step + n) % n;
        int recv_chunk_id = (idx - step - 1 + n) % n;
        
        // Calculate offsets and sizes
        int send_offset = send_chunk_id * chunk_size;
        int recv_offset = recv_chunk_id * chunk_size;
        
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
        memcpy(send_chunk, (char *)recvbuf + send_offset * dtype_size, send_bytes);
        
        // Transfer data using selected method (rendezvous or eager)
        if (transfer_data(pg_handle, send_chunk, recv_chunk,
                         (send_bytes > recv_bytes) ? send_bytes : recv_bytes) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Copy received chunk to result buffer
        memcpy((char *)recvbuf + recv_offset * dtype_size, recv_chunk, recv_bytes);
        
        // Synchronize before next step
        if (synchronize_servers(pg_handle) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
    }
    
    free(send_chunk);
    free(recv_chunk);
    
    return 0;
}