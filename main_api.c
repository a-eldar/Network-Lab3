// main_api.c
#include "rdma_allreduce.h"

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle) {
    if (!serverlist || len <= 0 || idx < 0 || idx >= len || !pg_handle) {
        fprintf(stderr, "Invalid parameters for connect_process_group\n");
        return -1;
    }
    
    // Initialize PGHandle structure
    memset(pg_handle, 0, sizeof(PGHandle));
    pg_handle->num_processes = len;
    pg_handle->my_rank = idx;
    pg_handle->buffer_size = DEFAULT_BUFFER_SIZE;
    pg_handle->tcp_listen_fd = -1;
    pg_handle->tcp_client_fd = -1;
    
    printf("Initializing process group: rank %d of %d processes\n", idx, len);
    
    // Allocate working buffer
    pg_handle->work_buffer = malloc(pg_handle->buffer_size);
    if (!pg_handle->work_buffer) {
        fprintf(stderr, "Failed to allocate working buffer\n");
        return -1;
    }
    memset(pg_handle->work_buffer, 0, pg_handle->buffer_size);
    
    // Setup RDMA connections for both neighbors
    printf("Setting up RDMA connections...\n");
    if (setup_rdma_connection(&pg_handle->left_conn, pg_handle->buffer_size) != 0) {
        fprintf(stderr, "Failed to setup left RDMA connection\n");
        free(pg_handle->work_buffer);
        return -1;
    }
    
    if (setup_rdma_connection(&pg_handle->right_conn, pg_handle->buffer_size) != 0) {
        fprintf(stderr, "Failed to setup right RDMA connection\n");
        cleanup_rdma_connection(&pg_handle->left_conn);
        free(pg_handle->work_buffer);
        return -1;
    }
    
    // Exchange RDMA connection information via TCP
    printf("Exchanging RDMA connection information...\n");
    if (exchange_rdma_info(pg_handle, serverlist, len, idx) != 0) {
        fprintf(stderr, "Failed to exchange RDMA info\n");
        cleanup_rdma_connection(&pg_handle->left_conn);
        cleanup_rdma_connection(&pg_handle->right_conn);
        free(pg_handle->work_buffer);
        return -1;
    }
    
    // Establish RDMA connections
    printf("Establishing RDMA connections...\n");
    if (establish_rdma_connections(pg_handle) != 0) {
        fprintf(stderr, "Failed to establish RDMA connections\n");
        cleanup_rdma_connection(&pg_handle->left_conn);
        cleanup_rdma_connection(&pg_handle->right_conn);
        free(pg_handle->work_buffer);
        return -1;
    }
    
    // Mark as initialized
    pg_handle->ring_initialized = 1;
    
    printf("Process group initialization completed successfully\n");
    return 0;
}

int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle) {
    if (!pg_handle || !pg_handle->ring_initialized) {
        fprintf(stderr, "PGHandle not initialized\n");
        return -1;
    }
    
    // Validate input parameters
    if (validate_input_parameters(sendbuf, recvbuf, count, datatype, op) != 0) {
        return -1;
    }
    
    size_t element_size = get_datatype_size(datatype);
    size_t total_size = count * element_size;
    
    // Check if data fits in our buffer
    if (total_size > pg_handle->buffer_size) {
        fprintf(stderr, "Data size (%zu bytes) exceeds buffer size (%zu bytes)\n", 
                total_size, pg_handle->buffer_size);
        return -1;
    }
    
    printf("Starting All-Reduce: rank %d, count=%d, datatype=%s, operation=%s\n",
           pg_handle->my_rank, count, 
           (datatype == INT) ? "INT" : "DOUBLE",
           (op == SUM) ? "SUM" : "MULT");
    
    // Phase 1: Reduce-Scatter
    printf("Phase 1: Reduce-Scatter\n");
    if (perform_ring_reduce_scatter(sendbuf, count, datatype, op, pg_handle) != 0) {
        fprintf(stderr, "Reduce-scatter phase failed\n");
        return -1;
    }
    
    // Phase 2: All-Gather
    printf("Phase 2: All-Gather\n");
    if (perform_ring_allgather(pg_handle->work_buffer, count, datatype, pg_handle) != 0) {
        fprintf(stderr, "All-gather phase failed\n");
        return -1;
    }
    
    // Copy final result to output buffer
    memcpy(recvbuf, pg_handle->work_buffer, total_size);
    
    printf("All-Reduce completed successfully\n");
    return 0;
}

int pg_close(PGHandle* pg_handle) {
    if (!pg_handle) {
        return -1;
    }
    
    printf("Closing process group for rank %d\n", pg_handle->my_rank);
    
    // Close TCP connections if still open
    if (pg_handle->tcp_listen_fd > 0) {
        close(pg_handle->tcp_listen_fd);
        pg_handle->tcp_listen_fd = -1;
    }
    
    if (pg_handle->tcp_client_fd > 0) {
        close(pg_handle->tcp_client_fd);
        pg_handle->tcp_client_fd = -1;
    }
    
    // Cleanup RDMA connections
    cleanup_rdma_connection(&pg_handle->left_conn);
    cleanup_rdma_connection(&pg_handle->right_conn);
    
    // Free working buffer
    if (pg_handle->work_buffer) {
        free(pg_handle->work_buffer);
        pg_handle->work_buffer = NULL;
    }
    
    // Reset the handle
    memset(pg_handle, 0, sizeof(PGHandle));
    
    printf("Process group closed successfully\n");
    return 0;
}

// Additional helper function for debugging
void pg_print_status(PGHandle* pg_handle) {
    if (!pg_handle) {
        printf("PGHandle is NULL\n");
        return;
    }
    
    printf("=== Process Group Status ===\n");
    printf("Rank: %d / %d\n", pg_handle->my_rank, pg_handle->num_processes);
    printf("Initialized: %s\n", pg_handle->ring_initialized ? "Yes" : "No");
    printf("Buffer size: %zu bytes\n", pg_handle->buffer_size);
    printf("Left connection: %s\n", pg_handle->left_conn.connected ? "Connected" : "Disconnected");
    printf("Right connection: %s\n", pg_handle->right_conn.connected ? "Connected" : "Disconnected");
    printf("TCP listen fd: %d\n", pg_handle->tcp_listen_fd);
    printf("TCP client fd: %d\n", pg_handle->tcp_client_fd);
    printf("============================\n");
}