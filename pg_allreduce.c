#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    // Simple barrier synchronization using RDMA writes
    int sync_val = 1;
    size_t sync_offset = pg_handle->work_buffer_size - sizeof(int);
    
    // Write sync value to right neighbor
    memcpy((char *)pg_handle->right_conn->buf + sync_offset, &sync_val, sizeof(int));
    if (post_send(pg_handle->right_conn, 
                       (char *)pg_handle->right_conn->buf + sync_offset,
                       sizeof(int),
                       pg_handle->right_conn->remote_addr + sync_offset,
                       pg_handle->right_conn->remote_rkey) < 0) {
        return -1;
    }
    
    if (wait_for_completion(pg_handle->right_conn) < 0) {
        return -1;
    }
    
    // Wait for sync from left neighbor
    int received_sync = 0;
    int attempts = 0;
    while (received_sync == 0 && attempts < 1000) {
        if (post_recv(pg_handle->left_conn, 
                          (char *)pg_handle->left_conn->buf + sync_offset,
                          sizeof(int)) < 0) {
            return -1;
        }
        
        if (wait_for_completion(pg_handle->left_conn) < 0) {
            return -1;
        }
        
        memcpy(&received_sync, (char *)pg_handle->left_conn->buf + sync_offset, sizeof(int));
        if (received_sync == 0) {
            usleep(1000); // Wait 1ms
        }
        attempts++;
    }
    
    // Clear sync flag for next use
    sync_val = 0;
    memcpy((char *)pg_handle->left_conn->buf + sync_offset, &sync_val, sizeof(int));
    memcpy((char *)pg_handle->right_conn->buf + sync_offset, &sync_val, sizeof(int));
    
    return 0;
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
    for (int step = 0; step < n; step++) {
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
        
        // Write to right neighbor's buffer
        memcpy(pg_handle->right_conn->buf, send_chunk, send_bytes);

        if (post_recv(pg_handle->left_conn, 
                          pg_handle->left_conn->buf,
                          recv_bytes) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }

        if (post_send(pg_handle->right_conn,
                           pg_handle->right_conn->buf,
                           send_bytes,
                           pg_handle->right_conn->remote_addr,
                           pg_handle->right_conn->remote_rkey) < 0) {
            free(send_chunk);
            free(recv_chunk); 
            return -1;
        }

        if (wait_for_completion(pg_handle->left_conn) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        if (wait_for_completion(pg_handle->right_conn) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // // Synchronize to ensure write is complete
        // if (synchronize_servers(pg_handle) < 0) {
        //     free(send_chunk);
        //     free(recv_chunk);
        //     return -1;
        // }

        sleep(1); // wait 1s for write to complete




        // -------- DEBUG PRINT --------
        // print the first of the the sent chunk
        if (datatype == INT) {
            printf("Server %d: Sending chunk %d with first value %d || step = %d\n", idx, send_chunk_id, ((int *)send_chunk)[0], step);
        } else if (datatype == DOUBLE) {
            printf("Server %d: Sending chunk %d with first value %f || step = %d\n", idx, send_chunk_id, ((double *)send_chunk)[0], step);
        }
        // ------------------------------
        
        

        
        memcpy(recv_chunk, pg_handle->left_conn->buf, recv_bytes);
        
        // Perform reduction operation
        perform_operation((char *)recvbuf + recv_offset * dtype_size,
                         recv_chunk,
                         recv_count,
                         datatype,
                         op);
        
        // // Synchronize before next step
        // if (synchronize_servers(pg_handle) < 0) {
        //     free(send_chunk);
        //     free(recv_chunk);
        //     return -1;
        // }

        sleep(1); // Wait 1s for read to complete


        // -------- DEBUG PRINT --------
        // print the first of the received chunk
        if (datatype == INT) {
            printf("Server %d: Received chunk %d with first value %d || step = %d\n", idx
                     , recv_chunk_id, ((int *)recv_chunk)[0], step);
        } else if (datatype == DOUBLE) {
            printf("Server %d: Received chunk %d with first value %f || step = %d\n", idx
                        , recv_chunk_id, ((double *)recv_chunk)[0], step);
        }
        // ------------------------------
    }
    // -------- DEBUG PRINT --------
    // print the first of each chunk
    for (int i = 0; i < n; i++) {
        int offset = i * chunk_size;
        if (i == n - 1) {
            offset += remainder; // Last chunk may have remainder
        }
        if (offset < count) {
            if (datatype == INT) {
                printf("Server %d: Final result chunk %d starts with %d\n", idx, i, ((int *)recvbuf)[offset]);
            } else if (datatype == DOUBLE) {
                printf("Server %d: Final result chunk %d starts with %f\n", idx, i, ((double *)recvbuf)[offset]);
            }
        }
    }
    // ------------------------------
    
    // Phase 2: All-gather using ring algorithm
    // Each server broadcasts its chunk to all others
    for (int step = 0; step < n; step++) {
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
        
        // Write to right neighbor's buffer
        memcpy(pg_handle->right_conn->buf, send_chunk, send_bytes);
        if (post_send(pg_handle->right_conn,
                           pg_handle->right_conn->buf,
                           send_bytes,
                           pg_handle->right_conn->remote_addr,
                           pg_handle->right_conn->remote_rkey) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        if (wait_for_completion(pg_handle->right_conn) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Synchronize to ensure write is complete
        // if (synchronize_servers(pg_handle) < 0) {
        //     free(send_chunk);
        //     free(recv_chunk);
        //     return -1;
        // }
        sleep(1); // wait 1s for write to complete
        
        // Read from left neighbor's buffer
        if (post_recv(pg_handle->left_conn, 
                          pg_handle->left_conn->buf,
                          recv_bytes) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        if (wait_for_completion(pg_handle->left_conn) < 0) {
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        memcpy(recv_chunk, pg_handle->left_conn->buf, recv_bytes);
        
        // Copy received chunk to result buffer
        memcpy((char *)recvbuf + recv_offset * dtype_size, recv_chunk, recv_bytes);
        
        // Synchronize before next step
        // if (synchronize_servers(pg_handle) < 0) {
        //     free(send_chunk);
        //     free(recv_chunk);
        //     return -1;
        // }
        sleep(1); // Wait 1s for read to complete
    }
    
    free(send_chunk);
    free(recv_chunk);
    
    return 0;
}