#include "pg_handle.h"
#include "rdma_utils.h"
#include "tcp_exchange.h"
#include "ring_allreduce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_BUFFER_SIZE (1 << 20) // 1MB default buffer size

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle) {
    if (!serverlist || len <= 0 || idx < 0 || idx >= len || !pg_handle) {
        fprintf(stderr, "Invalid parameters to connect_process_group\n");
        return -1;
    }
    
    memset(pg_handle, 0, sizeof(PGHandle));
    
    // Initialize basic parameters
    pg_handle->rank = idx;
    pg_handle->size = len;
    pg_handle->max_buffer_size = MAX_BUFFER_SIZE;
    pg_handle->ib_port = DEFAULT_IB_PORT;
    pg_handle->page_size = sysconf(_SC_PAGESIZE);
    
    // Copy server list
    pg_handle->serverlist = malloc(len * sizeof(char*));
    if (!pg_handle->serverlist) {
        fprintf(stderr, "Failed to allocate memory for server list\n");
        return -1;
    }
    
    for (int i = 0; i < len; i++) {
        pg_handle->serverlist[i] = strdup(serverlist[i]);
        if (!pg_handle->serverlist[i]) {
            fprintf(stderr, "Failed to duplicate server name %d\n", i);
            // Cleanup already allocated strings
            for (int j = 0; j < i; j++) {
                free(pg_handle->serverlist[j]);
            }
            free(pg_handle->serverlist);
            return -1;
        }
    }
    
    // Get IB device list
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB devices list\n");
        pg_close(pg_handle);
        return -1;
    }
    
    pg_handle->ib_dev = *dev_list;
    if (!pg_handle->ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    // Initialize RDMA connections
    if (init_neighbor_connection(&pg_handle->left_neighbor, pg_handle->ib_dev, 
                                pg_handle->max_buffer_size, pg_handle->ib_port, 0) != 0) {
        fprintf(stderr, "Failed to initialize left neighbor connection\n");
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    if (init_neighbor_connection(&pg_handle->right_neighbor, pg_handle->ib_dev, 
                                pg_handle->max_buffer_size, pg_handle->ib_port, 1) != 0) {
        fprintf(stderr, "Failed to initialize right neighbor connection\n");
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    // Prepare connection destinations
    struct connection_dest my_left_dest, my_right_dest;
    struct ibv_port_attr port_attr;
    
    if (get_port_info(pg_handle->left_neighbor.context, pg_handle->ib_port, &port_attr) != 0) {
        fprintf(stderr, "Failed to get port info\n");
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    // Set up my connection destinations
    my_left_dest.lid = port_attr.lid;
    my_left_dest.qpn = pg_handle->left_neighbor.qp->qp_num;
    my_left_dest.psn = lrand48() & 0xffffff;
    memset(&my_left_dest.gid, 0, sizeof(my_left_dest.gid));
    // ---------DEBUG---------
    printf("Rank %d: Left neighbor LID: %04x, QPN: %06x, PSN: %06x\n", 
           idx, my_left_dest.lid, my_left_dest.qpn, my_left_dest.psn);
    // ------------------------
    
    my_right_dest.lid = port_attr.lid;
    my_right_dest.qpn = pg_handle->right_neighbor.qp->qp_num;  
    my_right_dest.psn = lrand48() & 0xffffff;
    memset(&my_right_dest.gid, 0, sizeof(my_right_dest.gid));
    // ---------DEBUG---------
    printf("Rank %d: Right neighbor LID: %04x, QPN: %06x, PSN: %06x\n", 
           idx, my_right_dest.lid, my_right_dest.qpn, my_right_dest.psn);
    // ------------------------

    // Calculate neighbor indices
    int left_idx = (idx - 1 + len) % len;
    int right_idx = (idx + 1) % len;
    
    // TCP connection exchange phase
    struct connection_dest *left_dest = NULL, *right_dest = NULL;
    
    if (idx == 0) {
        // First server: send to left, then listen from right
        printf("Rank 0: Connecting to left neighbor %s\n", serverlist[left_idx]);
        left_dest = exchange_with_left(serverlist[left_idx], DEFAULT_PORT + left_idx, &my_right_dest);
        if (!left_dest) {
            fprintf(stderr, "Failed to exchange with left neighbor\n");
            ibv_free_device_list(dev_list);
            pg_close(pg_handle);
            return -1;
        }
        
        printf("Rank 0: Listening for right neighbor\n");
        right_dest = exchange_with_right(DEFAULT_PORT + idx, &my_left_dest);
        if (!right_dest) {
            fprintf(stderr, "Failed to exchange with right neighbor\n");
            free(left_dest);
            ibv_free_device_list(dev_list);
            pg_close(pg_handle);
            return -1;
        }
    } else {
        // Other servers: listen from left, then send to right
        printf("Rank %d: Listening for left neighbor\n", idx);
        left_dest = exchange_with_right(DEFAULT_PORT + idx, &my_left_dest);
        if (!left_dest) {
            fprintf(stderr, "Failed to exchange with left neighbor\n");
            ibv_free_device_list(dev_list);
            pg_close(pg_handle);
            return -1;
        }
        
        printf("Rank %d: Connecting to right neighbor %s\n", idx, serverlist[right_idx]);
        right_dest = exchange_with_left(serverlist[right_idx], DEFAULT_PORT + right_idx, &my_right_dest);
        if (!right_dest) {
            fprintf(stderr, "Failed to exchange with right neighbor\n");
            free(left_dest);
            ibv_free_device_list(dev_list);
            pg_close(pg_handle);
            return -1;
        }
    }
    
    // Store remote connection info
    pg_handle->left_neighbor.lid = left_dest->lid;
    pg_handle->left_neighbor.qpn = left_dest->qpn;
    pg_handle->left_neighbor.psn = left_dest->psn;
    pg_handle->left_neighbor.gid = left_dest->gid;
    // ---------DEBUG---------
    printf("Rank %d: Left neighbor connected: LID: %04x, Q
PN: %06x, PSN: %06x\n", 
           idx, pg_handle->left_neighbor.lid, pg_handle->left_neighbor.qpn, pg_handle->left_neighbor.psn);
    // ------------------------
    
    pg_handle->right_neighbor.lid = right_dest->lid;
    pg_handle->right_neighbor.qpn = right_dest->qpn;
    pg_handle->right_neighbor.psn = right_dest->psn;
    pg_handle->right_neighbor.gid = right_dest->gid;
    // ---------DEBUG---------
    printf("Rank %d: Right neighbor connected: LID: %04x, QPN: %06x, PSN: %06x\n", 
           idx, pg_handle->right_neighbor.lid, pg_handle->right_neighbor.qpn, pg_handle->right_neighbor.psn);
    // ------------------------
    
    // Connect QPs
    if (connect_qp(&pg_handle->left_neighbor, pg_handle->ib_port, 
                   my_left_dest.psn, left_dest, 0) != 0) {
        fprintf(stderr, "Failed to connect left QP\n");
        free(left_dest);
        free(right_dest);
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    if (connect_qp(&pg_handle->right_neighbor, pg_handle->ib_port, 
                   my_right_dest.psn, right_dest, 0) != 0) {
        fprintf(stderr, "Failed to connect right QP\n");
        free(left_dest);
        free(right_dest);
        ibv_free_device_list(dev_list);
        pg_close(pg_handle);
        return -1;
    }
    
    printf("Rank %d: Successfully connected to neighbors\n", idx);
    
    free(left_dest);
    free(right_dest);
    ibv_free_device_list(dev_list);
    
    return 0;
}

int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, 
                  OPERATION op, PGHandle* pg_handle) {
    if (!sendbuf || !recvbuf || count <= 0 || !pg_handle) {
        fprintf(stderr, "Invalid parameters to pg_all_reduce\n");
        return -1;
    }
    
    size_t elem_size = get_datatype_size(datatype);
    if (elem_size == 0) {
        fprintf(stderr, "Invalid datatype\n");
        return -1;
    }
    
    size_t total_size = count * elem_size;
    if (total_size > pg_handle->max_buffer_size) {
        fprintf(stderr, "Data size %zu exceeds buffer size %d\n", 
                total_size, pg_handle->max_buffer_size);
        return -1;
    }
    
    return perform_ring_allreduce(sendbuf, recvbuf, count, datatype, op, pg_handle);
}

int pg_close(PGHandle* pg_handle) {
    if (!pg_handle) {
        return -1;
    }
    
    int ret = 0;
    
    // Cleanup neighbor connections
    if (cleanup_neighbor_connection(&pg_handle->left_neighbor) != 0) {
        ret = -1;
    }
    
    if (cleanup_neighbor_connection(&pg_handle->right_neighbor) != 0) {
        ret = -1;
    }
    
    // Free server list
    if (pg_handle->serverlist) {
        for (int i = 0; i < pg_handle->size; i++) {
            if (pg_handle->serverlist[i]) {
                free(pg_handle->serverlist[i]);
            }
        }
        free(pg_handle->serverlist);
        pg_handle->serverlist = NULL;
    }
    
    memset(pg_handle, 0, sizeof(PGHandle));
    
    return ret;
}