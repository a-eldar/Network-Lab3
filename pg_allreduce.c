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

// static int synchronize_servers(PGHandle *pg_handle) {
//     // Simple barrier synchronization
//     int sync_val = 1;
//     size_t sync_offset = pg_handle->work_buffer_size - sizeof(int);
    
//     // Write sync value locally
//     memcpy((char *)pg_handle->left_conn->buf + sync_offset, &sync_val, sizeof(int));
    
//     // Wait for left neighbor to set their sync flag by reading from them
//     int received_sync = 0;
//     int attempts = 0;
//     void *local_sync_buf = malloc(sizeof(int));
    
//     while (received_sync == 0 && attempts < 1000) {
//         // Read sync flag from left neighbor's buffer
//         if (post_rdma_read(pg_handle->right_conn,
//                           local_sync_buf,
//                           sizeof(int),
//                           pg_handle->left_conn->remote_addr + sync_offset,
//                           pg_handle->left_conn->remote_rkey) < 0) {
//             free(local_sync_buf);
//             return -1;
//         }
        
//         if (wait_for_completion(pg_handle->right_conn) < 0) {
//             free(local_sync_buf);
//             return -1;
//         }
        
//         memcpy(&received_sync, local_sync_buf, sizeof(int));
//         if (received_sync == 0) {
//             usleep(1000); // Wait 1ms
//         }
//         attempts++;
//     }
    
//     // Clear sync flag for next use
//     sync_val = 0;
//     memcpy((char *)pg_handle->left_conn->buf + sync_offset, &sync_val, sizeof(int));
    
//     free(local_sync_buf);
//     return 0;
// }

// Rendezvous method: Local write + remote read
static int transfer_data_rendezvous(PGHandle *pg_handle) {
    
    if(rdma_write_to_right(pg_handle)){
        fprintf(stderr, "Rank %d: rdma_write_to_right failed\n", pg_handle->rank);
        return 1;
    }
    // Wait for completion
    if(poll_for_completion(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: poll_for_completion failed\n", pg_handle->rank);
        return 1;
    }

    // Small delay to let neighbors write
    sleep(5);

    // Print what we received in our recvbuf (left neighbor should have written here)
    printf("Rank %d: Received buffer = \"%s\"\n", pg_handle->rank, (char *)pg_handle->recvbuf);

    
    return 0;
}

// Eager method: RDMA Write (for easy swapping)
// static int transfer_data_eager(PGHandle *pg_handle, void *send_data, void *recv_data, size_t size) {
//     // Write data to right neighbor's buffer
//     if (post_rdma_write(pg_handle->right_conn,
//                        send_data,
//                        size,
//                        pg_handle->right_conn->remote_addr,
//                        pg_handle->right_conn->remote_rkey) < 0) {
//         return -1;
//     }
    
//     if (wait_for_completion(pg_handle->right_conn) < 0) {
//         return -1;
//     }
    
//     // Synchronize to ensure write is complete
//     if (synchronize_servers(pg_handle) < 0) {
//         return -1;
//     }
    
//     // Read from local buffer where left neighbor wrote
//     memcpy(recv_data, pg_handle->left_conn->buf, size);
    
//     return 0;
// }

// Wrapper function to easily switch between methods
// static inline int transfer_data(PGHandle *pg_handle, void *send_data, void *recv_data, size_t size) {
// #if USE_RENDEZVOUS_METHOD
//     return transfer_data_rendezvous(pg_handle, send_data, recv_data, size);
// #else
//     return transfer_data_eager(pg_handle, send_data, recv_data, size);
// #endif
// }

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
        transfer_data_rendezvous(pg_handle);

        // DEBUG
        // print the received chunk
        printf("Rank %d: After transfer, received chunk %d: ", pg_handle->rank, recv_chunk_id);
        for (int i = 0; i < recv_count; i++) {
            if (datatype == INT) {
                printf("%d ", ((int *)rdma_recvbuf)[i]);
            } else if (datatype == DOUBLE) {
                printf("%f ", ((double *)rdma_recvbuf)[i]);
            }
        }

        memcpy(temp_buf, rdma_recvbuf, recv_bytes);
        
        // Perform reduction operation
        perform_operation((char *)recvbuf + recv_offset,
                         temp_buf,
                         recv_count,
                         datatype,
                         op);
    }
    // print all reduced result
    printf("Rank %d: After reduce-scatter, recvbuf = ", pg_handle->rank);
    for (int i = 0; i < count; i++) {
        if (datatype == INT) {
            printf("%d ", ((int *)recvbuf)[i]);
        } else if (datatype == DOUBLE) {
            printf("%f ", ((double *)recvbuf)[i]);
        }
    }
    printf("\n");
    
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
        transfer_data_rendezvous(pg_handle);
        
        // Copy received chunk to result buffer
        memcpy((char *)recvbuf + recv_offset, rdma_recvbuf, recv_bytes);
        
    }
    
    return 0;
}