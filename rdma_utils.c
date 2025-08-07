#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#define TCP_PORT_BASE 18515

int init_rdma_connection(RDMAConnection *conn) {
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    int num_devices;
    
    // Get device list
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB devices list\n");
        return -1;
    }
    
    if (num_devices == 0) {
        fprintf(stderr, "No IB devices found\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    // Use first device
    ib_dev = dev_list[0];
    conn->context = ibv_open_device(ib_dev);
    if (!conn->context) {
        fprintf(stderr, "Failed to open device\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    
    ibv_free_device_list(dev_list);
    
    // Query port attributes
    if (ibv_query_port(conn->context, 1, &conn->port_attr)) {
        fprintf(stderr, "Failed to query port\n");
        return -1;
    }
    
    return 0;
}

int setup_rdma_resources(RDMAConnection *conn) {
    // Allocate Protection Domain
    conn->pd = ibv_alloc_pd(conn->context);
    if (!conn->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return -1;
    }
    
    // Allocate buffer
    conn->buf_size = RDMA_BUFFER_SIZE;
    conn->buf = malloc(conn->buf_size);
    if (!conn->buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }
    memset(conn->buf, 0, conn->buf_size);
    
    // Register memory region
    conn->mr = ibv_reg_mr(conn->pd, conn->buf, conn->buf_size,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!conn->mr) {
        fprintf(stderr, "Failed to register MR\n");
        return -1;
    }
    
    // Create Completion Queue
    conn->cq = ibv_create_cq(conn->context, 100, NULL, NULL, 0);
    if (!conn->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        return -1;
    }
    
    // Create Queue Pair
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = conn->cq;
    qp_init_attr.recv_cq = conn->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 100;
    qp_init_attr.cap.max_recv_wr = 100;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    
    conn->qp = ibv_create_qp(conn->pd, &qp_init_attr);
    if (!conn->qp) {
        fprintf(stderr, "Failed to create QP\n");
        return -1;
    }
    
    return 0;
}

int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return -1;
    }
    
    return 0;
}

int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid, uint32_t remote_psn) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = remote_psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;
    
    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return -1;
    }
    
    return 0;
}

int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    
    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return -1;
    }
    
    return 0;
}

int post_rdma_write(RDMAConnection *conn, void *local_addr, size_t size, uint64_t remote_addr, uint32_t remote_rkey) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;
    
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)local_addr;
    sge.length = size;
    sge.lkey = conn->mr->lkey;
    
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = remote_rkey;
    
    if (ibv_post_send(conn->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post RDMA write\n");
        return -1;
    }
    
    return 0;
}

int post_rdma_read(RDMAConnection *conn, void *local_addr, size_t size, uint64_t remote_addr, uint32_t remote_rkey) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;
    
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)local_addr;
    sge.length = size;
    sge.lkey = conn->mr->lkey;
    
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 2;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = remote_rkey;
    
    if (ibv_post_send(conn->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post RDMA read\n");
        return -1;
    }
    
    return 0;
}

int wait_for_completion(RDMAConnection *conn) {
    struct ibv_wc wc;
    int ne;
    
    do {
        ne = ibv_poll_cq(conn->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Failed to poll CQ\n");
            return -1;
        }
    } while (ne < 1);
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion failed with status %s (%d)\n",
                ibv_wc_status_str(wc.status), wc.status);
        return -1;
    }
    
    return 0;
}

void cleanup_rdma_connection(RDMAConnection *conn) {
    if (conn->qp) ibv_destroy_qp(conn->qp);
    if (conn->cq) ibv_destroy_cq(conn->cq);
    if (conn->mr) ibv_dereg_mr(conn->mr);
    if (conn->buf) free(conn->buf);
    if (conn->pd) ibv_dealloc_pd(conn->pd);
    if (conn->context) ibv_close_device(conn->context);
}

int get_server_ip(const char *hostname, char *ip_str, size_t ip_str_size) {
    struct hostent *host_entry;
    struct in_addr addr;
    
    host_entry = gethostbyname(hostname);
    if (!host_entry) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", hostname);
        return -1;
    }
    
    addr.s_addr = *((unsigned long *)host_entry->h_addr_list[0]);
    strncpy(ip_str, inet_ntoa(addr), ip_str_size - 1);
    ip_str[ip_str_size - 1] = '\0';
    
    return 0;
}

int exchange_connection_info_as_sender(const char *hostname, int port, ConnectionInfo *local_info, ConnectionInfo *remote_info) {
    int sock;
    struct sockaddr_in server_addr;
    char ip_str[INET_ADDRSTRLEN];
    int attempts = 0;
    const int max_attempts = 100;
    
    if (get_server_ip(hostname, ip_str, sizeof(ip_str)) < 0) {
        return -1;
    }
    
    // Keep trying to connect
    while (attempts < max_attempts) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            fprintf(stderr, "Failed to create socket\n");
            return -1;
        }
        
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = inet_addr(ip_str);
        
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            // Connection successful, exchange info
            if (send(sock, local_info, sizeof(ConnectionInfo), 0) != sizeof(ConnectionInfo)) {
                close(sock);
                return -1;
            }
            
            if (recv(sock, remote_info, sizeof(ConnectionInfo), 0) != sizeof(ConnectionInfo)) {
                close(sock);
                return -1;
            }
            
            close(sock);
            return 0;
        }
        
        close(sock);
        attempts++;
        usleep(100000); // Wait 100ms before retry
    }
    
    fprintf(stderr, "Failed to connect after %d attempts\n", max_attempts);
    return -1;
}

int exchange_connection_info_as_receiver(int port, ConnectionInfo *local_info, ConnectionInfo *remote_info) {
    int listen_sock, conn_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;
    
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return -1;
    }
    
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        close(listen_sock);
        return -1;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(listen_sock);
        return -1;
    }
    
    if (listen(listen_sock, 1) < 0) {
        close(listen_sock);
        return -1;
    }
    
    conn_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
    if (conn_sock < 0) {
        close(listen_sock);
        return -1;
    }
    
    // Exchange info
    if (recv(conn_sock, remote_info, sizeof(ConnectionInfo), 0) != sizeof(ConnectionInfo)) {
        close(conn_sock);
        close(listen_sock);
        return -1;
    }
    
    if (send(conn_sock, local_info, sizeof(ConnectionInfo), 0) != sizeof(ConnectionInfo)) {
        close(conn_sock);
        close(listen_sock);
        return -1;
    }
    
    close(conn_sock);
    close(listen_sock);
    return 0;
}