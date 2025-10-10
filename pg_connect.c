
#include "pg_connect.h"


////////////////////////// Helpers //////////////////////////


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
        for (int attempt = 0; attempt < PG_TCP_CONN_ATTEMPTS; ++attempt) { // Try 20 times
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

    printf("[ERROR] Failed to connect to %s:%d after 20 attempts\n", ip_str, port);
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

static int open_rdma_device(PGHandle *pg_handle) {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get RDMA devices list\n");
        return -1;
    }
    pg_handle->ctx = ibv_open_device(dev_list[0]); // Open the first device
    ibv_free_device_list(dev_list);
    if (!pg_handle->ctx) {
        fprintf(stderr, "Failed to open RDMA device\n");
        return -1;
    }
    return 0;
}
 



// Helper: Allocate and initialize PGHandle
static PGHandle* allocate_pg_handle(char **server_list, int size, int rank) {
    PGHandle *handle = (PGHandle *)calloc(1, sizeof(PGHandle));
    if (!handle) return NULL;
    handle->rank = rank;
    handle->num_servers = size;
    handle->servernames = server_list;
    handle->remote_rkeys = calloc(size, sizeof(uint32_t));
    handle->remote_addrs = calloc(size, sizeof(uintptr_t));
    return handle;
}

// Helper: Setup RDMA device, PD, CQ, QPs
static int setup_rdma_resources(PGHandle *handle) {
    if (open_rdma_device(handle) != 0) return -1;
    handle->pd = ibv_alloc_pd(handle->ctx);
    if (!handle->pd) return -1;
    handle->cq = ibv_create_cq(handle->ctx, 16, NULL, NULL, 0);
    if (!handle->cq) return -1;
    handle->qps = calloc(2, sizeof(struct ibv_qp *));
    if (!handle->qps) return -1;
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = handle->cq,
        .recv_cq = handle->cq,
        .cap = {
            .max_send_wr = 16,
            .max_recv_wr = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    for (int i = 0; i < 2; ++i) {
        handle->qps[i] = ibv_create_qp(handle->pd, &qp_init_attr);
        if (!handle->qps[i]) return -1;
    }
    return 0;
}

// Helper: Exchange QP info with neighbors
static int exchange_qp_info(PGHandle *handle, qp_info_t myinfo[2], qp_info_t *left_info, qp_info_t *right_info) {
    struct ibv_port_attr port_attr;
    ibv_query_port(handle->ctx, 1, &port_attr);
    for (int i = 0; i < 2; ++i) {
        myinfo[i].lid = port_attr.lid;
        myinfo[i].qpn = handle->qps[i]->qp_num;
        myinfo[i].psn = 100 + handle->rank * 10 + i;
    }
    int left = (handle->rank - 1 + handle->num_servers) % handle->num_servers;
    int right = (handle->rank + 1) % handle->num_servers;
    int sock_left, sock_right;
    if (handle->rank == 0) {
        sock_right = tcp_connect(handle->servernames[right], QP_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->num_servers));
        if (sock_right < 0) return -1;
        write(sock_right, &myinfo[1], sizeof(qp_info_t));
        read(sock_right, right_info, sizeof(qp_info_t));
        close(sock_right);
        sock_left = tcp_listen_accept(QP_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) return -1;
        read(sock_left, left_info, sizeof(qp_info_t));
        write(sock_left, &myinfo[0], sizeof(qp_info_t));
        close(sock_left);
    } else {
        sock_left = tcp_listen_accept(QP_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) return -1;
        read(sock_left, left_info, sizeof(qp_info_t));
        write(sock_left, &myinfo[0], sizeof(qp_info_t));
        close(sock_left);
        sock_right = tcp_connect(handle->servernames[right], QP_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->num_servers));
        if (sock_right < 0) return -1;
        write(sock_right, &myinfo[1], sizeof(qp_info_t));
        read(sock_right, right_info, sizeof(qp_info_t));
        close(sock_right);
    }
    return 0;
}

// Helper: Register send/recv buffers
static int register_buffers(PGHandle *handle) {
    handle->bufsize = RDMA_BUFFER_SIZE;
    handle->sendbuf = malloc(handle->bufsize);
    if (!handle->sendbuf) return -1;
    handle->mr_send = ibv_reg_mr(
        handle->pd,
        handle->sendbuf,
        handle->bufsize,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->mr_send) return -1;
    handle->local_rkey = handle->mr_send->rkey;
    handle->local_addr = (uintptr_t)handle->sendbuf;
    handle->recvbuf = malloc(handle->bufsize);
    if (!handle->recvbuf) return -1;
    handle->mr_recv = ibv_reg_mr(
        handle->pd,
        handle->recvbuf,
        handle->bufsize,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    );
    if (!handle->mr_recv) return -1;
    return 0;
}

// Helper: Exchange memory region info
static int exchange_mr_info(PGHandle *handle) {
    // Calculate left and right neighbor indices in the ring
    int left = (handle->rank - 1 + handle->num_servers) % handle->num_servers;
    int right = (handle->rank + 1) % handle->num_servers;

    // Prepare my memory region info to send
    mr_info_t my_mrinfo, right_mrinfo, left_mrinfo;
    my_mrinfo.rkey = handle->mr_recv->rkey;
    my_mrinfo.addr = (uintptr_t)handle->recvbuf;

    int sock_left, sock_right;

    // Rank 0: connect to right neighbor first, then accept from left
    // Other ranks: accept from left first, then connect to right
    if (handle->rank == 0) {
        // Connect to right neighbor and exchange MR info
        sock_right = tcp_connect(handle->servernames[right], MR_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->num_servers));
        if (sock_right < 0) return -1;
        // Send my MR info, receive right neighbor's MR info
        write(sock_right, &my_mrinfo, sizeof(mr_info_t));
        read(sock_right, &right_mrinfo, sizeof(mr_info_t));
        close(sock_right);
        // Store right neighbor's MR info
        handle->remote_rkeys[right] = right_mrinfo.rkey;
        handle->remote_addrs[right] = right_mrinfo.addr;

        // Accept connection from left neighbor and exchange MR info
        sock_left = tcp_listen_accept(MR_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) return -1;
        // Receive left neighbor's MR info, send my MR info
        read(sock_left, &left_mrinfo, sizeof(mr_info_t));
        write(sock_left, &my_mrinfo, sizeof(mr_info_t));
        close(sock_left);
        // Store left neighbor's MR info
        handle->remote_rkeys[left] = left_mrinfo.rkey;
        handle->remote_addrs[left] = left_mrinfo.addr;
    } else {
        // Accept connection from left neighbor and exchange MR info
        sock_left = tcp_listen_accept(MR_EXCHANGE_PORT_BASE + handle->rank);
        if (sock_left < 0) return -1;
        // Receive left neighbor's MR info, send my MR info
        read(sock_left, &left_mrinfo, sizeof(mr_info_t));
        write(sock_left, &my_mrinfo, sizeof(mr_info_t));
        close(sock_left);
        // Store left neighbor's MR info
        handle->remote_rkeys[left] = left_mrinfo.rkey;
        handle->remote_addrs[left] = left_mrinfo.addr;

        // Connect to right neighbor and exchange MR info
        sock_right = tcp_connect(handle->servernames[right], MR_EXCHANGE_PORT_BASE + ((handle->rank + 1) % handle->num_servers));
        if (sock_right < 0) return -1;
        // Send my MR info, receive right neighbor's MR info
        write(sock_right, &my_mrinfo, sizeof(mr_info_t));
        read(sock_right, &right_mrinfo, sizeof(mr_info_t));
        close(sock_right);
        // Store right neighbor's MR info
        handle->remote_rkeys[right] = right_mrinfo.rkey;
        handle->remote_addrs[right] = right_mrinfo.addr;
    }

    // Success
    return 0;
}

// Helper: Final resource check
static int final_resource_check(PGHandle *handle) {
    if (!handle->ctx || !handle->pd || !handle->cq || !handle->qps ||
        !handle->mr_send || !handle->mr_recv || !handle->sendbuf || !handle->recvbuf ||
        !handle->remote_rkeys || !handle->remote_addrs) {
        return -1;
    }
    return 0;
}

int connect_process_group(char **server_list, int size, void **pg_handle, int rank) {
    PGHandle *handle = allocate_pg_handle(server_list, size, rank);
    if (!handle) {
        for (int i = 0; i < size; ++i) free(server_list[i]);
        free(server_list);
        return -1;
    }
    *pg_handle = handle;
    if (setup_rdma_resources(handle) != 0) {
        pg_close(handle);
        return -1;
    }
    qp_info_t myinfo[2], left_info, right_info;
    if (exchange_qp_info(handle, myinfo, &left_info, &right_info) != 0) {
        pg_close(handle);
        return -1;
    }
    if (register_buffers(handle) != 0) {
        pg_close(handle);
        return -1;
    }
    if (connect_qp(handle->qps[0], &myinfo[0], &left_info)) {
        pg_close(handle);
        fprintf(stderr, "Failed to connect left QP\n");
        return -1;
    }
    if (connect_qp(handle->qps[1], &myinfo[1], &right_info)) {
        pg_close(handle);
        fprintf(stderr, "Failed to connect right QP\n");
        return -1;
    }
    if (exchange_mr_info(handle) != 0) {
        pg_close(handle);
        return -1;
    }
    if (final_resource_check(handle) != 0) {
        fprintf(stderr, "Resource allocation or registration failed\n");
        pg_close(handle);
        return -1;
    }
    return 0;
}
