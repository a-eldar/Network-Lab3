#include "ring_allreduce.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t get_datatype_size(DATATYPE datatype) {
    switch (datatype) {
        case INT: return sizeof(int);
        case DOUBLE: return sizeof(double);
        default: return 0;
    }
}

int get_chunk_size(int total_count, int rank, int size) {
    int base_chunk = total_count / size;
    int remainder = total_count % size;
    
    if (rank < remainder) {
        return base_chunk + 1;
    } else {
        return base_chunk;
    }
}

int get_chunk_offset(int total_count, int rank, int size) {
    int base_chunk = total_count / size;
    int remainder = total_count % size;
    int offset = 0;
    
    for (int i = 0; i < rank; i++) {
        if (i < remainder) {
            offset += base_chunk + 1;
        } else {
            offset += base_chunk;
        }
    }
    
    return offset;
}

void perform_operation(void* a, void* b, int count, DATATYPE datatype, OPERATION op) {
    if (datatype == INT) {
        int *ia = (int*)a;
        int *ib = (int*)b;
        
        if (op == SUM) {
            for (int i = 0; i < count; i++) {
                ia[i] += ib[i];
            }
        } else if (op == MULT) {
            for (int i = 0; i < count; i++) {
                ia[i] *= ib[i];
            }
        }
    } else if (datatype == DOUBLE) {
        double *da = (double*)a;
        double *db = (double*)b;
        
        if (op == SUM) {
            for (int i = 0; i < count; i++) {
                da[i] += db[i];
            }
        } else if (op == MULT) {
            for (int i = 0; i < count; i++) {
                da[i] *= db[i];
            }
        }
    }
}

int perform_ring_allreduce(void* sendbuf, void* recvbuf, int count, 
                          DATATYPE datatype, OPERATION op, PGHandle* pg_handle) {
    size_t elem_size = get_datatype_size(datatype);
    size_t total_size = count * elem_size;
    
    // Copy sendbuf to recvbuf initially
    memcpy(recvbuf, sendbuf, total_size);
    
    // Allocate temporary buffers for communication
    void *send_chunk = malloc(pg_handle->max_buffer_size);
    void *recv_chunk = malloc(pg_handle->max_buffer_size);
    
    if (!send_chunk || !recv_chunk) {
        fprintf(stderr, "Failed to allocate temporary buffers\n");
        free(send_chunk);
        free(recv_chunk);
        return -1;
    }
    
    // Phase 1: Reduce-scatter
    // Each process will reduce its assigned chunk
    for (int step = 0; step < pg_handle->size; step++) {
        // Calculate which chunk to work on in this step
        int chunk_rank = (pg_handle->rank - step + pg_handle->size) % pg_handle->size;
        int chunk_offset = get_chunk_offset(count, chunk_rank, pg_handle->size);
        int chunk_size = get_chunk_size(count, chunk_rank, pg_handle->size);
        size_t chunk_bytes = chunk_size * elem_size;
        
        // Prepare the chunk to send
        char *recv_ptr = (char*)recvbuf + chunk_offset * elem_size;
        memcpy(send_chunk, recv_ptr, chunk_bytes);
        memcpy(pg_handle->right_neighbor.buf, send_chunk, chunk_bytes);
        
        // Post receive for incoming data (except in last step)
        if (step < pg_handle->size - 1) {
            if (post_recv(&pg_handle->left_neighbor)) {
                fprintf(stderr, "Failed to post receive in reduce-scatter step %d\n", step);
                free(send_chunk);
                free(recv_chunk);
                return -1;
            }
        }
        
        // Send data to right neighbor
        if (post_send(&pg_handle->right_neighbor, chunk_bytes)) {
            fprintf(stderr, "Failed to post send in reduce-scatter step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Wait for send completion
        if (wait_for_completion(&pg_handle->right_neighbor, 1)) {
            fprintf(stderr, "Failed to wait for send completion in reduce-scatter step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Wait for receive completion and perform reduction (except in last step)
        if (step < pg_handle->size - 1) {
            if (wait_for_completion(&pg_handle->left_neighbor, 1)) {
                fprintf(stderr, "Failed to wait for receive completion in reduce-scatter step %d\n", step);
                free(send_chunk);
                free(recv_chunk);
                return -1;
            }
            
            // Copy received data and perform reduction
            memcpy(recv_chunk, pg_handle->left_neighbor.buf, chunk_bytes);
            perform_operation(recv_ptr, recv_chunk, chunk_size, datatype, op);
        }
    }
    
    // Phase 2: Allgather
    // Distribute the reduced chunks to all processes
    for (int step = 0; step < pg_handle->size - 1; step++) {
        // Calculate which chunk to work on in this step
        int chunk_rank = (pg_handle->rank - step + pg_handle->size - 1) % pg_handle->size;
        int chunk_offset = get_chunk_offset(count, chunk_rank, pg_handle->size);
        int chunk_size = get_chunk_size(count, chunk_rank, pg_handle->size);
        size_t chunk_bytes = chunk_size * elem_size;
        
        // Prepare the chunk to send
        char *recv_ptr = (char*)recvbuf + chunk_offset * elem_size;
        memcpy(pg_handle->right_neighbor.buf, recv_ptr, chunk_bytes);
        
        // Post receive for incoming data
        if (post_recv(&pg_handle->left_neighbor)) {
            fprintf(stderr, "Failed to post receive in allgather step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Send data to right neighbor
        if (post_send(&pg_handle->right_neighbor, chunk_bytes)) {
            fprintf(stderr, "Failed to post send in allgather step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Wait for send completion
        if (wait_for_completion(&pg_handle->right_neighbor, 1)) {
            fprintf(stderr, "Failed to wait for send completion in allgather step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Wait for receive completion and copy data
        if (wait_for_completion(&pg_handle->left_neighbor, 1)) {
            fprintf(stderr, "Failed to wait for receive completion in allgather step %d\n", step);
            free(send_chunk);
            free(recv_chunk);
            return -1;
        }
        
        // Calculate where to store the received chunk
        int received_chunk_rank = (pg_handle->rank - step + pg_handle->size - 2) % pg_handle->size;
        int received_chunk_offset = get_chunk_offset(count, received_chunk_rank, pg_handle->size);
        char *received_ptr = (char*)recvbuf + received_chunk_offset * elem_size;
        
        // Copy received data
        memcpy(received_ptr, pg_handle->left_neighbor.buf, chunk_bytes);
    }
    
    free(send_chunk);
    free(recv_chunk);
    return 0;
}