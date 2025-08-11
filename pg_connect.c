#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TCP_PORT_BASE 18515

static int setup_connection(RDMAConnection *conn, ConnectionInfo *local_info) {
    if (init_rdma_connection(conn) < 0) {
        fprintf(stderr, "Failed to initialize RDMA connection\n");
        return -1;
    }
    
    if (setup_rdma_resources(conn) < 0) {
        fprintf(stderr, "Failed to setup RDMA resources\n");
        return -1;
    }
    
    if (modify_qp_to_init(conn->qp) < 0) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return -1;
    }
    
    // Fill local connection info
    local_info->lid = conn->port_attr.lid;
    local_info->qpn = conn->qp->qp_num;
    local_info->psn = 0;
    local_info->addr = (uintptr_t)conn->buf;
    local_info->rkey = conn->mr->rkey;
    
    return 0;
}

static int establish_connection(RDMAConnection *conn, ConnectionInfo *remote_info) {
    // Store remote info
    conn->remote_qpn = remote_info->qpn;
    conn->remote_lid = remote_info->lid;
    conn->remote_psn = remote_info->psn;
    conn->remote_addr = remote_info->addr;
    conn->remote_rkey = remote_info->rkey;
    
    // Modify QP to RTR
    if (modify_qp_to_rtr(conn->qp, remote_info->qpn, remote_info->lid, remote_info->psn) < 0) {
        return -1;
    }
    
    // Modify QP to RTS
    if (modify_qp_to_rts(conn->qp) < 0) {
        return -1;
    }
    
    return 0;
}

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle) {
    if (!serverlist || len < 2 || idx < 0 || idx >= len || !pg_handle) {
        fprintf(stderr, "Invalid parameters\n");
        return -1;
    }
    
    // Initialize PGHandle
    memset(pg_handle, 0, sizeof(PGHandle));
    pg_handle->num_servers = len;
    pg_handle->server_idx = idx;
    pg_handle->server_list = serverlist;
    
    // Allocate connections
    pg_handle->left_conn = (RDMAConnection *)calloc(1, sizeof(RDMAConnection));
    pg_handle->right_conn = (RDMAConnection *)calloc(1, sizeof(RDMAConnection));
    
    if (!pg_handle->left_conn || !pg_handle->right_conn) {
        fprintf(stderr, "Failed to allocate connections\n");
        return -1;
    }
    
    // Calculate neighbor indices
    int left_idx = (idx - 1 + len) % len;
    int right_idx = (idx + 1) % len;
    
    ConnectionInfo local_left_info, remote_left_info;
    ConnectionInfo local_right_info, remote_right_info;
    
    // Setup RDMA resources for both connections
    // Left connection: for writing data that left neighbor will read
    if (setup_connection(pg_handle->left_conn, &local_left_info) < 0) {
        fprintf(stderr, "Failed to setup left connection\n");
        return -1;
    }
    
    // Right connection: for reading data from left neighbor
    if (setup_connection(pg_handle->right_conn, &local_right_info) < 0) {
        fprintf(stderr, "Failed to setup right connection\n");
        return -1;
    }
    
    printf("Server %d: Setting up connections with left=%d and right=%d\n", idx, left_idx, right_idx);
    
    // Phase 1: Exchange with left neighbor
    // Server 0 sends to its left (server n-1), others receive from their right
    if (idx == 0) {
        // Server 0 sends to its left neighbor
        if (exchange_connection_info_as_sender(serverlist[left_idx], 
                                              TCP_PORT_BASE + left_idx,
                                              &local_left_info,
                                              &remote_left_info) < 0) {
            fprintf(stderr, "Server %d: Failed to exchange with left neighbor\n", idx);
            return -1;
        }
    } else {
        // Other servers receive from their right neighbor
        if (exchange_connection_info_as_receiver(TCP_PORT_BASE + idx,
                                                &local_left_info,
                                                &remote_left_info) < 0) {
            fprintf(stderr, "Server %d: Failed to exchange with right neighbor\n", idx);
            return -1;
        }
    }
    
    // Phase 2: Exchange with right neighbor  
    // Server 0 sends to its right (server 1), others receive from their left
    if (idx == 0) {
        // Server 0 sends to its right neighbor
        if (exchange_connection_info_as_sender(serverlist[right_idx],
                                              TCP_PORT_BASE + right_idx + len, // Use different port
                                              &local_right_info,
                                              &remote_right_info) < 0) {
            fprintf(stderr, "Server %d: Failed to exchange with right neighbor\n", idx);
            return -1;
        }
    } else {
        // Other servers receive from their left neighbor
        if (exchange_connection_info_as_receiver(TCP_PORT_BASE + idx + len, // Use different port
                                                &local_right_info,
                                                &remote_right_info) < 0) {
            fprintf(stderr, "Server %d: Failed to exchange with left neighbor\n", idx);
            return -1;
        }
    }
    
    // Store remote info for left connection (we will read from left neighbor)
    pg_handle->left_conn->remote_qpn = remote_left_info.qpn;
    pg_handle->left_conn->remote_lid = remote_left_info.lid;
    pg_handle->left_conn->remote_psn = remote_left_info.psn;
    pg_handle->left_conn->remote_addr = remote_left_info.addr;
    pg_handle->left_conn->remote_rkey = remote_left_info.rkey;
    
    // Store remote info for right connection (right neighbor will read from us)
    pg_handle->right_conn->remote_qpn = remote_right_info.qpn;
    pg_handle->right_conn->remote_lid = remote_right_info.lid;
    pg_handle->right_conn->remote_psn = remote_right_info.psn;
    pg_handle->right_conn->remote_addr = remote_right_info.addr;
    pg_handle->right_conn->remote_rkey = remote_right_info.rkey;
    
    // Establish RDMA connections
    if (establish_connection(pg_handle->left_conn, &remote_left_info) < 0) {
        fprintf(stderr, "Failed to establish left connection\n");
        return -1;
    }
    
    if (establish_connection(pg_handle->right_conn, &remote_right_info) < 0) {
        fprintf(stderr, "Failed to establish right connection\n");
        return -1;
    }
    
    // Allocate work buffer for ring algorithm
    pg_handle->work_buffer_size = RDMA_BUFFER_SIZE;
    pg_handle->work_buffer = malloc(pg_handle->work_buffer_size);
    if (!pg_handle->work_buffer) {
        fprintf(stderr, "Failed to allocate work buffer\n");
        return -1;
    }
    memset(pg_handle->work_buffer, 0, pg_handle->work_buffer_size);
    
    pg_handle->connected = 1;
    
    printf("Server %d: Successfully connected to process group\n", idx);
    printf("  - Left connection buffer for neighbor %d to read from\n", left_idx);
    printf("  - Right connection to read from neighbor %d\n", left_idx);
    
    return 0;
}