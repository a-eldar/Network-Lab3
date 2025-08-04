// tcp_setup.c
#include "rdma_allreduce.h"

int setup_tcp_server(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 1) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

int connect_tcp_client(const char *hostname, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Resolve hostname to IP
    char *ip = resolve_hostname(hostname);
    if (!ip) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", hostname);
        close(sockfd);
        return -1;
    }
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip);
        close(sockfd);
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

static int send_rdma_info(int sockfd, rdma_connection_t *conn) {
    rdma_info_t info;
    memset(&info, 0, sizeof(info));
    
    // Fill RDMA connection information
    info.rkey = conn->recv_mr->rkey;
    info.addr = (uint64_t)conn->recv_buf;
    info.qp_num = conn->qp->qp_num;
    
    // Get port attributes for LID
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn->context, 1, &port_attr) == 0) {
        info.lid = port_attr.lid;
    }
    
    // Get GID
    if (ibv_query_gid(conn->context, 1, 0, &info.gid) != 0) {
        fprintf(stderr, "Failed to query GID\n");
        return -1;
    }
    
    // Send the information
    ssize_t bytes_sent = send(sockfd, &info, sizeof(info), 0);
    if (bytes_sent != sizeof(info)) {
        perror("send");
        return -1;
    }
    
    return 0;
}

static int recv_rdma_info(int sockfd, rdma_connection_t *conn) {
    rdma_info_t info;
    
    // Receive the information
    ssize_t bytes_recv = recv(sockfd, &info, sizeof(info), MSG_WAITALL);
    if (bytes_recv != sizeof(info)) {
        perror("recv");
        return -1;
    }
    
    // Store remote connection information
    conn->remote_rkey = info.rkey;
    conn->remote_addr = info.addr;
    
    // Store other connection info for QP establishment
    // In a full implementation, you would use this info to properly
    // establish the RDMA connection
    
    return 0;
}

int exchange_rdma_info(PGHandle *pg_handle, char **serverlist, int len, int idx) {
    if (!pg_handle || !serverlist || len <= 0 || idx < 0 || idx >= len) {
        return -1;
    }
    
    int left_idx = (idx - 1 + len) % len;
    int right_idx = (idx + 1) % len;
    
    // Determine TCP port (could be more sophisticated)
    int tcp_port = DEFAULT_TCP_PORT + idx;
    
    if (idx == 0) {
        // First server: send to left (last server), then listen for right
        
        // Connect to left neighbor and send our info
        pg_handle->tcp_client_fd = connect_tcp_client(serverlist[left_idx], 
                                                     DEFAULT_TCP_PORT + left_idx);
        if (pg_handle->tcp_client_fd < 0) {
            fprintf(stderr, "Failed to connect to left neighbor\n");
            return -1;
        }
        
        if (send_rdma_info(pg_handle->tcp_client_fd, &pg_handle->right_conn) != 0) {
            fprintf(stderr, "Failed to send RDMA info to left neighbor\n");
            close(pg_handle->tcp_client_fd);
            return -1;
        }
        
        close(pg_handle->tcp_client_fd);
        pg_handle->tcp_client_fd = -1;
        
        // Listen for connection from right neighbor
        pg_handle->tcp_listen_fd = setup_tcp_server(tcp_port);
        if (pg_handle->tcp_listen_fd < 0) {
            fprintf(stderr, "Failed to setup TCP server\n");
            return -1;
        }
        
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(pg_handle->tcp_listen_fd, 
                              (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        if (recv_rdma_info(client_fd, &pg_handle->left_conn) != 0) {
            fprintf(stderr, "Failed to receive RDMA info from right neighbor\n");
            close(client_fd);
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        close(client_fd);
        
    } else {
        // Other servers: listen first, then send
        
        // Listen for connection from left neighbor
        pg_handle->tcp_listen_fd = setup_tcp_server(tcp_port);
        if (pg_handle->tcp_listen_fd < 0) {
            fprintf(stderr, "Failed to setup TCP server\n");
            return -1;
        }
        
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(pg_handle->tcp_listen_fd, 
                              (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        if (recv_rdma_info(client_fd, &pg_handle->left_conn) != 0) {
            fprintf(stderr, "Failed to receive RDMA info from left neighbor\n");
            close(client_fd);
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        close(client_fd);
        
        // Connect to right neighbor and send our info
        pg_handle->tcp_client_fd = connect_tcp_client(serverlist[right_idx], 
                                                     DEFAULT_TCP_PORT + right_idx);
        if (pg_handle->tcp_client_fd < 0) {
            fprintf(stderr, "Failed to connect to right neighbor\n");
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        if (send_rdma_info(pg_handle->tcp_client_fd, &pg_handle->right_conn) != 0) {
            fprintf(stderr, "Failed to send RDMA info to right neighbor\n");
            close(pg_handle->tcp_client_fd);
            close(pg_handle->tcp_listen_fd);
            return -1;
        }
        
        close(pg_handle->tcp_client_fd);
        pg_handle->tcp_client_fd = -1;
    }
    
    // Close the listening socket
    if (pg_handle->tcp_listen_fd > 0) {
        close(pg_handle->tcp_listen_fd);
        pg_handle->tcp_listen_fd = -1;
    }
    
    return 0;
}