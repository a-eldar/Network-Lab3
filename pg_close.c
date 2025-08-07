#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdlib.h>
#include <stdio.h>

int pg_close(PGHandle* pg_handle) {
    if (!pg_handle) {
        return -1;
    }
    
    // Clean up RDMA connections
    if (pg_handle->left_conn) {
        cleanup_rdma_connection(pg_handle->left_conn);
        free(pg_handle->left_conn);
        pg_handle->left_conn = NULL;
    }
    
    if (pg_handle->right_conn) {
        cleanup_rdma_connection(pg_handle->right_conn);
        free(pg_handle->right_conn);
        pg_handle->right_conn = NULL;
    }
    
    // Free work buffer
    if (pg_handle->work_buffer) {
        free(pg_handle->work_buffer);
        pg_handle->work_buffer = NULL;
    }
    
    // Reset connection status
    pg_handle->connected = 0;
    
    printf("Server %d: Closed process group connections\n", pg_handle->server_idx);
    
    return 0;
}