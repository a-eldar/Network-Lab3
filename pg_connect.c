
#include "pg_connect.h"
static int page_size;


////////////////////////// Helpers //////////////////////////
// Helper: Resolve hostname to IP address
static char* get_ip_from_hostname(const char *hostname) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;  // IPv4 only
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        printf("[DEBUG] Failed to resolve hostname: %s\n", hostname);
        return NULL;  // Failed to resolve
    }

    struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
    char *ip = strdup(inet_ntoa(addr->sin_addr));
    freeaddrinfo(res);
    return ip;
}

// Connect to a hostname:port, return socket fd
static int tcp_connect(const char *hostname, int port) {
    // Resolve hostname to IP
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;  // IPv4 only
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        printf("[DEBUG] Failed to resolve hostname: %s\n", hostname);
        return -1;
    }

    struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);

    // Now connect using the IP
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res_connect;
    if (getaddrinfo(ip_str, portstr, &hints, &res_connect) != 0) {
        printf("[ERROR] Failed to getaddrinfo for IP: %s\n", ip_str);
        freeaddrinfo(res);
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *rp = res_connect; rp; rp = rp->ai_next) {
        for (int attempt = 0; attempt < 10; ++attempt) { // Try 10 times
            sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sock == -1) continue;
            if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
                freeaddrinfo(res_connect);
                freeaddrinfo(res);
                return sock;
            }
            close(sock); sock = -1;
            usleep(2000000); // Wait 2s before retry
        }
    }

    printf("[ERROR] Failed to connect to %s:%d after 10 attempts\n", ip_str, port);
    freeaddrinfo(res_connect);
    freeaddrinfo(res);
    return -1;
}

// Listen on a port, return accepted socket fd
static int tcp_listen_accept(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(sock, 1);
    int client = accept(sock, NULL, NULL);
    close(sock);
    return client;
}






/////////////////////////// Main Functions //////////////////////////

// Helper: Parse comma-separated server list into array
// We expect the server list to be a comma-separated list of hostnames or IP addresses
/**
 * @brief Parse a comma-separated list of hostnames or IP addresses into an array of strings
 * @param list: comma-separated list of hostnames or IP addresses (e.g. "server1,server2,server3")
 * @param out_names: pointer to pointer to array of strings that will be set to the array of strings
 * @param out_count: pointer to integer that will be set to the number of servers
 * @return 0 on success, -1 on failure
 */
static int parse_server_list(const char *list, char ***out_names, int *out_count) {
    if (!list || !out_names || !out_count) return -1;
    // Count commas to determine number of servers
    int count = 1;
    for (const char *p = list; *p; ++p) if (*p == ',') ++count;
    char **names = (char **)calloc(count, sizeof(char *));
    if (!names) return -1;
    char *copy = strdup(list);  // strdup duplicates the string pointed to by list
    if (!copy) { free(names); return -1; }
    int idx = 0;
    char *token = strtok(copy, ",");
    while (token && idx < count) {
        names[idx++] = strdup(token);
        token = strtok(NULL, ",");
    }
    free(copy);
    *out_names = names;
    *out_count = count;
    return 0;
}

// Helper to clean up all RDMA resources in pg_handle
/**
 * @brief Clean up all RDMA resources in the process group pg_handle
 * @param pg_handle: pointer to the process group pg_handle
 */
static void cleanup_pg_handle(pg_handle_t *pg_handle) {
    if (!pg_handle) return;
    if (pg_handle->mr_send) ibv_dereg_mr(pg_handle->mr_send);
    if (pg_handle->mr_recv) ibv_dereg_mr(pg_handle->mr_recv);
    if (pg_handle->sendbuf) free(pg_handle->sendbuf);
    if (pg_handle->recvbuf) free(pg_handle->recvbuf);
    if (pg_handle->qps) {
        for (int i = 0; i < 2; ++i) {
            if (pg_handle->qps[i]) ibv_destroy_qp(pg_handle->qps[i]);
        }
        free(pg_handle->qps);
    }
    if (pg_handle->cq) ibv_destroy_cq(pg_handle->cq);
    if (pg_handle->pd) ibv_dealloc_pd(pg_handle->pd);
    if (pg_handle->ctx) ibv_close_device(pg_handle->ctx);
    if (pg_handle->remote_rkeys) free(pg_handle->remote_rkeys);
    if (pg_handle->remote_addrs) free(pg_handle->remote_addrs);
    if (pg_handle->servernames) {
        for (int i = 0; i < pg_handle->size; ++i) free(pg_handle->servernames[i]);
        free(pg_handle->servernames);
    }
    free(pg_handle);
}

// Helper to transition a QP to RTR(ready to receive) and RTS(ready to send)
/**
 * @brief Connect a QP to a remote peer
 * @param qp: pointer to the QP to connect
 * @param local: local QP info (lid, qpn, psn)
 * @param remote: remote QP info (lid, qpn, psn)
 * @return 0 on success, -1 on failure
 */
static int connect_qp(struct ibv_qp *qp, qp_info_t *local, qp_info_t *remote) {
    struct ibv_qp_attr attr;
    int flags;

    // INIT
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, flags)) {
        perror("Failed to move QP to INIT");
        return -1;
    }

    // RTR
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = remote->qpn;
    attr.rq_psn = remote->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    memset(&attr.ah_attr, 0, sizeof(attr.ah_attr));
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote->lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags)) {
        perror("Failed to modify QP to RTR");
        return -1;
    }

    // Move to RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = local->psn;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(qp, &attr, flags)) {
        perror("Failed to modify QP to RTS");
        fprintf(stderr, "errno: %d\n", errno);
        return -1;
    }
    return 0;
}

// Connect processes in a ring and set up RDMA resources
/**
 * @brief Connect processes in a ring and set up RDMA resources.
 * This function initializes RDMA resources, connects to neighbors, and prepares the process group pg_handle.
 * @param servername: name of the server to connect to
 * @param pg_handle: pointer to the process group pg_handle
 * @return 0 on success, -1 on failure
 */
int connect_process_group(char *servername, void **pg_handle, int rank) {
    // Parse server list
    char **server_list = NULL;
    int size = 0;
    if (parse_server_list(servername, &server_list, &size) != 0) {
        fprintf(stderr, "Failed to parse server list\n");
        return -1;
    }

    // Allocate and fill handle - this is a pointer to a struct that will be used to store the RDMA resources for this process
    pg_handle_t *handle = (pg_handle_t *)calloc(1, sizeof(pg_handle_t));
    if (!handle) {
        for (int i = 0; i < size; ++i) free(names[i]);
        free(names);
        return -1;
    }
    handle->rank = rank;
    handle->size = size;
    handle->servernames = names;
    handle->remote_rkeys = calloc(size, sizeof(uint32_t));
    handle->remote_addrs = calloc(size, sizeof(uintptr_t));

    *pg_handle = handle;

    // 1. Open RDMA device (get ibv_context)
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "No RDMA devices found\n");
        return -1;
    }
    handle->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!handle->ctx) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to open RDMA device\n");
        return -1;
    }

    // 2. Allocate Protection Domain
    handle->pd = ibv_alloc_pd(handle->ctx);
    if (!handle->pd) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to allocate PD\n");
        return -1;
    }

    // 3. Create Completion Queue
    handle->cq = ibv_create_cq(handle->ctx, 16, NULL, NULL, 0);
    if (!handle->cq) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to create CQ\n");
        return -1;
    }

    // 3.5. Create QPs for left and right neighbors in the ring
    handle->qps = calloc(2, sizeof(struct ibv_qp *)); // [0]=left, [1]=right
    if (!handle->qps) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to allocate QP array\n");
        return -1;
    }
    struct ibv_qp_init_attr qp_init_attr = {
            .send_cq = handle->cq,
            .recv_cq = handle->cq,
            .cap = {
                    .max_send_wr = 16,
                    .max_recv_wr = 16,
                    .max_send_sge = 1,
                    .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_RC, // Reliable Connection
    };
    for (int i = 0; i < 2; ++i) {
        handle->qps[i] = ibv_create_qp(handle->pd, &qp_init_attr);
        if (!handle->qps[i]) {
            cleanup_pg_handle(handle);
            fprintf(stderr, "Failed to create QP %d\n", i);
            // Cleanup omitted for brevity
            return -1;
        }
    }

    // Exchange QP info with neighbors

    int left = (handle->rank - 1 + handle->size) % handle->size;
    int right = (handle->rank + 1) % handle->size;

    // Gather local QP info
    struct ibv_port_attr port_attr;
    ibv_query_port(handle->ctx, 1, &port_attr);  // 1 is the port number of the RDMA device
    qp_info_t myinfo[2]; // holds the QP information for your own process.
    // myinfo[0] is the left QP (QP you use to communicate with your left neighbor), myinfo[1] is the right QP (QP you use to communicate with your right neighbor)
    // Loop through the two QPs (left and right) and set the LID, QPN, and PSN
    for (int i = 0; i < 2; ++i) {
        myinfo[i].lid = port_attr.lid;
        myinfo[i].qpn = handle->qps[i]->qp_num;
        myinfo[i].psn = 100 + handle->rank * 10 + i; // Rank-based unique PSNs
    }


    int sock_left;
    int sock_right;
    qp_info_t right_info;
    qp_info_t left_info;

    if (handle->rank == 0) {
        // Rank 0: connect first, then accept  (to avoid deadlock)
        sock_right = tcp_connect(handle->servernames[right], QP_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->size));
        if (sock_right < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_connect right");
            return -1;
        }
        write(sock_right, &myinfo[1], sizeof(qp_info_t));
        read(sock_right, &right_info, sizeof(qp_info_t));
        close(sock_right);

        sock_left = tcp_listen_accept(QP_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_listen_accept left");
            return -1;
        }
        read(sock_left, &left_info, sizeof(qp_info_t));
        write(sock_left, &myinfo[0], sizeof(qp_info_t));
        close(sock_left);
    } else {
        // All other ranks: accept first, then connect
        int sock_left = tcp_listen_accept(QP_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_listen_accept left");
            return -1;
        }
        read(sock_left, &left_info, sizeof(qp_info_t));
        write(sock_left, &myinfo[0], sizeof(qp_info_t));
        close(sock_left);

        sock_right = tcp_connect(handle->servernames[right], QP_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->size));
        if (sock_right < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_connect right");
            return -1;
        }
        write(sock_right, &myinfo[1], sizeof(qp_info_t));
        read(sock_right, &right_info, sizeof(qp_info_t));
        close(sock_right);
    }


    // 4. Allocate and register buffer
    handle->bufsize = 1024*1024; // was originally 4096
    handle->sendbuf = malloc(handle->bufsize);
    if (!handle->sendbuf) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to allocate sendbuf\n");
        return -1;
    }
    handle->mr_send = ibv_reg_mr(
            handle->pd,
            handle->sendbuf,
            handle->bufsize,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->mr_send) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to register memory region\n");
        return -1;
    }
    handle->local_rkey = handle->mr_send->rkey;
    handle->local_addr = (uintptr_t)handle->sendbuf;

    // 4.5. Allocate and register recvbuf
    handle->recvbuf = malloc(handle->bufsize);
    if (!handle->recvbuf) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to allocate recvbuf\n");
        return -1;
    }
    handle->mr_recv = ibv_reg_mr(
            handle->pd,
            handle->recvbuf,
            handle->bufsize,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->mr_recv) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to register recvbuf memory region\n");
        return -1;
    }


    // For left QP (index 0)
    if (connect_qp(handle->qps[0], &myinfo[0], &left_info)) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to connect left QP\n");
        return -1;
    }

    // For right QP (index 1)
    if (connect_qp(handle->qps[1], &myinfo[1], &right_info)) {
        cleanup_pg_handle(handle);
        fprintf(stderr, "Failed to connect right QP\n");
        return -1;
    }

    // Exchange memory region info (rkey, addr) with neighbors
    mr_info_t my_mrinfo, right_mrinfo, left_mrinfo;
    my_mrinfo.rkey = handle->mr_recv->rkey;
    my_mrinfo.addr = (uintptr_t)handle->recvbuf;

    if (handle->rank == 0){  // connect first, then accept
        sock_right = tcp_connect(handle->servernames[right], MR_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->size)); // Use a different port for MR exchange
        if (sock_right < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_connect right (MR)");
            return -1;
        }
        write(sock_right, &my_mrinfo, sizeof(mr_info_t)); // Send our MR info
        read(sock_right, &right_mrinfo, sizeof(mr_info_t)); // Receive right neighbor's MR info
        close(sock_right);

        handle->remote_rkeys[right] = right_mrinfo.rkey;
        handle->remote_addrs[right] = right_mrinfo.addr;

        sock_left = tcp_listen_accept(MR_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_listen_accept left (MR)");
            return -1;
        }
        read(sock_left, &left_mrinfo, sizeof(mr_info_t)); // Receive left neighbor's MR info
        write(sock_left, &my_mrinfo, sizeof(mr_info_t));  // Send our MR info
        close(sock_left);

        handle->remote_rkeys[left] = left_mrinfo.rkey;
        handle->remote_addrs[left] = left_mrinfo.addr;
    }
    else{  // accept first, connect second
        sock_left = tcp_listen_accept(MR_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_listen_accept left (MR)");
            return -1;
        }
        read(sock_left, &left_mrinfo, sizeof(mr_info_t)); // Receive left neighbor's MR info
        write(sock_left, &my_mrinfo, sizeof(mr_info_t));  // Send our MR info
        close(sock_left);

        handle->remote_rkeys[left] = left_mrinfo.rkey;
        handle->remote_addrs[left] = left_mrinfo.addr;

        sock_right = tcp_connect(handle->servernames[right], MR_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->size)); // Use a different port for MR exchange
        if (sock_right < 0) {
            cleanup_pg_handle(handle);
            perror("tcp_connect right (MR)");
            return -1;
        }
        write(sock_right, &my_mrinfo, sizeof(mr_info_t)); // Send our MR info
        read(sock_right, &right_mrinfo, sizeof(mr_info_t)); // Receive right neighbor's MR info
        close(sock_right);

        handle->remote_rkeys[right] = right_mrinfo.rkey;
        handle->remote_addrs[right] = right_mrinfo.addr;
    }

    // Check all pointers
    if (!handle->ctx || !handle->pd || !handle->cq || !handle->qps ||
        !handle->mr_send || !handle->mr_recv || !handle->sendbuf || !handle->recvbuf ||
        !handle->remote_rkeys || !handle->remote_addrs) {
        fprintf(stderr, "Resource allocation or registration failed\n");
        cleanup_pg_handle(handle);
        return -1;
    }
    return 0;
}

/**
 * @brief Close the process group and free all resources
 * @param pg_handle: pointer to the process group pg_handle
 * @return 0 on success, -1 on failure
 */
int pg_close(void *pg_handle) {
    pg_handle_t *handle = (pg_handle_t *)pg_handle;
    if (!handle) return -1;
    cleanup_pg_handle(handle);
    return 0;
}