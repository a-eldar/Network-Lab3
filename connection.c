#include "connection.h"

int connect_process_group(char **server_list, int idx, int servers_num, void **pg_handle_ptr) {
    if (!server_list || !pg_handle_ptr) {
        fprintf(stderr, "Invalid parameters to connect_process_group\n");
        return -1;
    }

    // Allocate and initialize pg_handle
    pg_handle *handle = calloc(1, sizeof(pg_handle));
    if (!handle) {
        fprintf(stderr, "Failed to allocate pg_handle\n");
        return -1;
    }

    // Get my hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        free(handle);
        return -1;
    }
    
    handle->my_hostname = strdup(hostname);
    if (!handle->my_hostname) {
        fprintf(stderr, "Failed to allocate hostname\n");
        free(handle);
        return -1;
    }

    // Parse server list and find my rank
    if (parse_server_list(server_list, &handle->server_names, &handle->num_processes) != 0) {
        fprintf(stderr, "Failed to parse server list\n");
        cleanup_on_error(handle);
        return -1;
    }

    handle->rank = find_my_rank(handle->server_names, handle->num_processes, hostname);
    if (handle->rank == -1) {
        fprintf(stderr, "Could not find hostname %s in server list\n", hostname);
        cleanup_on_error(handle);
        return -1;
    }

    printf("Process rank %d of %d (hostname: %s)\n", 
           handle->rank, handle->num_processes, hostname);

    // Setup RDMA device and resources
    if (setup_rdma_device(handle) != 0) {
        fprintf(stderr, "Failed to setup RDMA device\n");
        cleanup_on_error(handle);
        return -1;
    }

    // Create memory regions
    if (create_memory_regions(handle) != 0) {
        fprintf(stderr, "Failed to create memory regions\n");
        cleanup_on_error(handle);
        return -1;
    }

    // Create Queue Pairs
    if (create_queue_pairs(handle) != 0) {
        fprintf(stderr, "Failed to create queue pairs\n");
        cleanup_on_error(handle);
        return -1;
    }

    // Establish TCP connections and exchange RDMA info
    if (establish_tcp_connections(handle) != 0) {
        fprintf(stderr, "Failed to establish TCP connections\n");
        cleanup_on_error(handle);
        return -1;
    }

    // Exchange RDMA connection information
    if (exchange_rdma_info_tcp(handle) != 0) {
        fprintf(stderr, "Failed to exchange RDMA info\n");
        cleanup_on_error(handle);
        return -1;
    }

    // Connect Queue Pairs using exchanged information
    if (connect_qps(handle) != 0) {
        fprintf(stderr, "Failed to connect queue pairs\n");
        cleanup_on_error(handle);
        return -1;
    }

    handle->initialized = 1;
    *pg_handle_ptr = handle;
    
    printf("Process group connection established successfully\n");
    return 0;
}

static int parse_server_list(const char *server_list, char ***servers, int *count) {
    // Count servers by counting spaces + 1
    int server_count = 1;
    for (const char *p = server_list; *p; p++) {
        if (*p == ' ') server_count++;
    }

    // Allocate array of server name pointers
    char **server_array = malloc(server_count * sizeof(char*));
    if (!server_array) return -1;

    // Parse server names
    char *list_copy = strdup(server_list);
    if (!list_copy) {
        free(server_array);
        return -1;
    }

    char *token = strtok(list_copy, " ");
    int i = 0;
    while (token && i < server_count) {
        server_array[i] = strdup(token);
        if (!server_array[i]) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) free(server_array[j]);
            free(server_array);
            free(list_copy);
            return -1;
        }
        token = strtok(NULL, " ");
        i++;
    }

    free(list_copy);
    *servers = server_array;
    *count = server_count;
    return 0;
}


static int setup_rdma_device(pg_handle *handle) {
    // Get device list
    int num_devices;
    handle->device_list = ibv_get_device_list(&num_devices);
    if (!handle->device_list || num_devices == 0) {
        fprintf(stderr, "No InfiniBand devices found\n");
        return -1;
    }

    // Use first device
    handle->device = handle->device_list[0];
    
    // Open device context
    handle->context = ibv_open_device(handle->device);
    if (!handle->context) {
        fprintf(stderr, "Failed to open device context\n");
        return -1;
    }

    // Allocate Protection Domain
    handle->pd = ibv_alloc_pd(handle->context);
    if (!handle->pd) {
        fprintf(stderr, "Failed to allocate protection domain\n");
        return -1;
    }

    // Create Completion Queue
    handle->cq = ibv_create_cq(handle->context, 100, NULL, NULL, 0);
    if (!handle->cq) {
        fprintf(stderr, "Failed to create completion queue\n");
        return -1;
    }

    return 0;
}

static int create_memory_regions(pg_handle *handle) {
    // Calculate buffer size
    const size_t buffer_size = (BUFFER_SIZE >> 2) / sizeof(int) * sizeof(int);
    
    // Allocate buffers for left and right neighbors
    handle->left_neighbor.buffer_size = buffer_size;
    handle->left_neighbor.local_buffer = malloc(buffer_size);
    if (!handle->left_neighbor.local_buffer) {
        fprintf(stderr, "Failed to allocate left buffer\n");
        return -1;
    }

    handle->right_neighbor.buffer_size = buffer_size;
    handle->right_neighbor.local_buffer = malloc(buffer_size);
    if (!handle->right_neighbor.local_buffer) {
        fprintf(stderr, "Failed to allocate right buffer\n");
        return -1;
    }

    // Register memory regions
    handle->left_neighbor.local_mr = ibv_reg_mr(
        handle->pd, 
        handle->left_neighbor.local_buffer, 
        buffer_size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->left_neighbor.local_mr) {
        fprintf(stderr, "Failed to register left memory region\n");
        return -1;
    }

    handle->right_neighbor.local_mr = ibv_reg_mr(
        handle->pd,
        handle->right_neighbor.local_buffer,
        buffer_size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->right_neighbor.local_mr) {
        fprintf(stderr, "Failed to register right memory region\n");
        return -1;
    }

    // Allocate work buffer
    handle->work_buffer_size = buffer_size;
    handle->work_buffer = malloc(buffer_size);
    if (!handle->work_buffer) {
        fprintf(stderr, "Failed to allocate work buffer\n");
        return -1;
    }

    return 0;
}

static int create_queue_pairs(pg_handle *handle) {
    struct ibv_qp_init_attr qp_attr = {0};
    qp_attr.send_cq = handle->cq;
    qp_attr.recv_cq = handle->cq;
    qp_attr.qp_type = IBV_QPT_RC;  // Reliable Connection
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    // Create QP for left neighbor
    handle->left_neighbor.qp = ibv_create_qp(handle->pd, &qp_attr);
    if (!handle->left_neighbor.qp) {
        fprintf(stderr, "Failed to create left QP\n");
        return -1;
    }

    // Create QP for right neighbor
    handle->right_neighbor.qp = ibv_create_qp(handle->pd, &qp_attr);
    if (!handle->right_neighbor.qp) {
        fprintf(stderr, "Failed to create right QP\n");
        return -1;
    }

    return 0;
}


static int establish_tcp_connections(pg_handle *handle) {
    int listen_sock = -1;
    int left_sock = -1, right_sock = -1;
    struct sockaddr_in addr;
    
    // Create listening socket
    listen_sock = create_listening_socket();
    if (listen_sock < 0) {
        fprintf(stderr, "Failed to create listening socket\n");
        return -1;
    }
    if(setup_listening_socket(listen_sock, handle->rank, &addr)){
        fprintf(stderr, "Failed to setup listening socket\n");
        close(listen_sock);
        return -1;
    }

    // Connect to right neighbor and accept from left neighbor
    if (connect_left_and_right_neighbors(handle, listen_sock, &left_sock, &right_sock) != 0) {
        fprintf(stderr, "Failed to connect to neighbors\n");
        close(listen_sock);
        return -1;
    }

    close(listen_sock);


    // Store sockets temporarily (we'll close them after exchanging info)
    handle->left_neighbor.qp->qp_context = (void*)(intptr_t)left_sock;
    handle->right_neighbor.qp->qp_context = (void*)(intptr_t)right_sock;

    return 0;
}

static int exchange_rdma_info_tcp(pg_handle *handle) {
    // Get local port info
    struct ibv_port_attr port_attr;
    if (ibv_query_port(handle->context, 1, &port_attr) != 0) {
        fprintf(stderr, "Failed to query port\n");
        return -1;
    }

    // Get local GID
    union ibv_gid local_gid;
    if (ibv_query_gid(handle->context, 1, 0, &local_gid) != 0) {
        fprintf(stderr, "Failed to query GID\n");
        return -1;
    }

    // Prepare connection info for left neighbor
    rdma_conn_info_t left_info = {0};
    left_info.qpn = handle->left_neighbor.qp->qp_num;
    left_info.lid = port_attr.lid;
    left_info.gid = local_gid;
    left_info.addr = (uint64_t)handle->left_neighbor.local_buffer;
    left_info.rkey = handle->left_neighbor.local_mr->rkey;

    // Prepare connection info for right neighbor  
    rdma_conn_info_t right_info = {0};
    right_info.qpn = handle->right_neighbor.qp->qp_num;
    right_info.lid = port_attr.lid;
    right_info.gid = local_gid;
    right_info.addr = (uint64_t)handle->right_neighbor.local_buffer;
    right_info.rkey = handle->right_neighbor.local_mr->rkey;

    // Get TCP sockets
    int left_sock = (int)(intptr_t)handle->left_neighbor.qp->qp_context;
    int right_sock = (int)(intptr_t)handle->right_neighbor.qp->qp_context;

    // Exchange with left neighbor
    rdma_conn_info_t left_remote_info;
    if (send(left_sock, &left_info, sizeof(left_info), 0) != sizeof(left_info) ||
        recv(left_sock, &left_remote_info, sizeof(left_remote_info), MSG_WAITALL) != sizeof(left_remote_info)) {
        fprintf(stderr, "Failed to exchange info with left neighbor\n");
        return -1;
    }

    // Exchange with right neighbor
    rdma_conn_info_t right_remote_info;
    if (send(right_sock, &right_info, sizeof(right_info), 0) != sizeof(right_info) ||
        recv(right_sock, &right_remote_info, sizeof(right_remote_info), MSG_WAITALL) != sizeof(right_remote_info)) {
        fprintf(stderr, "Failed to exchange info with right neighbor\n");
        return -1;
    }

    // Store remote connection info
    handle->left_neighbor.remote_qpn = left_remote_info.qpn;
    handle->left_neighbor.remote_lid = left_remote_info.lid;
    handle->left_neighbor.remote_gid = left_remote_info.gid;
    handle->left_neighbor.remote_addr = left_remote_info.addr;
    handle->left_neighbor.remote_rkey = left_remote_info.rkey;

    handle->right_neighbor.remote_qpn = right_remote_info.qpn;
    handle->right_neighbor.remote_lid = right_remote_info.lid;
    handle->right_neighbor.remote_gid = right_remote_info.gid;
    handle->right_neighbor.remote_addr = right_remote_info.addr;
    handle->right_neighbor.remote_rkey = right_remote_info.rkey;

    // Close TCP connections
    close(left_sock);
    close(right_sock);

    // Clear the context pointers
    handle->left_neighbor.qp->qp_context = NULL;
    handle->right_neighbor.qp->qp_context = NULL;

    printf("RDMA connection info exchanged successfully\n");
    return 0;
}

static int connect_qps(pg_handle *handle) {
    struct ibv_qp_attr attr = {0};
    int flags;

    // Move QPs to INIT state
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(handle->left_neighbor.qp, &attr, flags) != 0 ||
        ibv_modify_qp(handle->right_neighbor.qp, &attr, flags) != 0) {
        fprintf(stderr, "Failed to move QPs to INIT state\n");
        return -1;
    }

    // Move QPs to RTR (Ready to Receive) state
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    // Connect left QP
    attr.dest_qp_num = handle->left_neighbor.remote_qpn;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = handle->left_neighbor.remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    if (ibv_modify_qp(handle->left_neighbor.qp, &attr, flags) != 0) {
        fprintf(stderr, "Failed to move left QP to RTR state\n");
        return -1;
    }

    // Connect right QP
    attr.dest_qp_num = handle->right_neighbor.remote_qpn;
    attr.ah_attr.dlid = handle->right_neighbor.remote_lid;

    if (ibv_modify_qp(handle->right_neighbor.qp, &attr, flags) != 0) {
        fprintf(stderr, "Failed to move right QP to RTR state\n");
        return -1;
    }

    // Move QPs to RTS (Ready to Send) state
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(handle->left_neighbor.qp, &attr, flags) != 0 ||
        ibv_modify_qp(handle->right_neighbor.qp, &attr, flags) != 0) {
        fprintf(stderr, "Failed to move QPs to RTS state\n");
        return -1;
    }

    printf("Queue Pairs connected successfully\n");
    return 0;
}

static void cleanup_on_error(pg_handle *handle) {
    if (!handle) return;

    if (handle->left_neighbor.local_mr) ibv_dereg_mr(handle->left_neighbor.local_mr);
    if (handle->right_neighbor.local_mr) ibv_dereg_mr(handle->right_neighbor.local_mr);
    if (handle->left_neighbor.local_buffer) free(handle->left_neighbor.local_buffer);
    if (handle->right_neighbor.local_buffer) free(handle->right_neighbor.local_buffer);
    if (handle->work_buffer) free(handle->work_buffer);
    if (handle->left_neighbor.qp) ibv_destroy_qp(handle->left_neighbor.qp);
    if (handle->right_neighbor.qp) ibv_destroy_qp(handle->right_neighbor.qp);
    if (handle->cq) ibv_destroy_cq(handle->cq);
    if (handle->pd) ibv_dealloc_pd(handle->pd);
    if (handle->context) ibv_close_device(handle->context);
    if (handle->device_list) ibv_free_device_list(handle->device_list);
    
    if (handle->server_names) {
        for (int i = 0; i < handle->num_processes; i++) {
            free(handle->server_names[i]);
        }
        free(handle->server_names);
    }
    if (handle->my_hostname) free(handle->my_hostname);
    
    free(handle);
}


static int create_listening_socket(void) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    return listen_sock;
}

static int setup_listening_socket(int listen_sock, int rank, struct sockaddr_in *addr) {
    // Bind to port
    memset(addr, 0, sizeof(*addr));
    addr->sin_addr.s_addr = INADDR_ANY;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(TCP_PORT_BASE + handle->rank);

    if (bind(listen_sock, (struct sockaddr*)addr, sizeof(*addr)) < 0) {
        perror("bind");
        return -1;
    }

    if (listen(listen_sock, 2) < 0) {
        perror("listen");
        return -1;
    }

    // Set non-blocking
    fcntl(listen_sock, F_SETFL, O_NONBLOCK);

    printf("Listening on port %d\n", TCP_PORT_BASE + rank);
    return 0;

}

static int connect_left_and_right_neighbors(pg_handle *handle, int listen_sock, int *left_sock, int *right_sock) {
    int left_rank = (handle->rank - 1 + handle->num_processes) % handle->num_processes;
    int right_rank = (handle->rank + 1) % handle->num_processes;

    // Try to connect to right neighbor and accept from left neighbor
    for (int retry = 0; retry < MAX_RETRIES && (*left_sock < 0 || *right_sock < 0); retry++) {
        // Try to accept connection from left neighbor
        if (*left_sock < 0) {
            *left_sock = accept(listen_sock, NULL, NULL);
            if (*left_sock >= 0) {
                printf("Accepted connection from left neighbor (rank %d)\n", left_rank);
            }
        }

        // Try to connect to right neighbor
        if (*right_sock < 0) {
            *right_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (*right_sock >= 0) {
                struct sockaddr_in right_addr;
                memset(&right_addr, 0, sizeof(right_addr));
                right_addr.sin_family = AF_INET;
                right_addr.sin_port = htons(TCP_PORT_BASE + right_rank);

                // Resolve hostname
                struct hostent *he = gethostbyname(handle->server_names[right_rank]);
                if (he) {
                    memcpy(&right_addr.sin_addr, he->h_addr_list[0], he->h_length);
                    
                    if (connect(*right_sock, (struct sockaddr*)&right_addr, sizeof(right_addr)) == 0) {
                        printf("Connected to right neighbor (rank %d)\n", right_rank);
                    } else {
                        close(*right_sock);
                        *right_sock = -1;
                    }
                } else {
                    close(*right_sock);
                    *right_sock = -1;
                }
            }
        }

        if (*left_sock < 0 || *right_sock < 0) {
            usleep(RETRY_DELAY_MS * 1000);
        }
    }
    if (*left_sock < 0 || *right_sock < 0) {
        fprintf(stderr, "Failed to establish TCP connections\n");
        if (left_sock >= 0) close(left_sock);
        if (right_sock >= 0) close(right_sock);
        return -1;
    }
    return 0;
}