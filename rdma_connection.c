// rdma_connection.c
#include "rdma_allreduce.h"

int setup_rdma_connection(rdma_connection_t *conn, size_t buf_size) {
    if (!conn || buf_size == 0) {
        return -1;
    }
    
    // Initialize connection structure
    memset(conn, 0, sizeof(rdma_connection_t));
    
    // Get RDMA device list
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get RDMA device list\n");
        return -1;
    }
    
    // Open device context (use first available device)
    conn->context = ibv_open_device(dev_list[0]);
    if (!conn->context) {
        fprintf(stderr, "Failed to open RDMA device\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    ibv_free_device_list(dev_list);
    
    // Allocate protection domain
    conn->pd = ibv_alloc_pd(conn->context);
    if (!conn->pd) {
        fprintf(stderr, "Failed to allocate protection domain\n");
        ibv_close_device(conn->context);
        return -1;
    }
    
    // Create completion queue
    conn->cq = ibv_create_cq(conn->context, MAX_WR * 2, NULL, NULL, 0);
    if (!conn->cq) {
        fprintf(stderr, "Failed to create completion queue\n");
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        return -1;
    }
    
    // Set buffer size
    conn->buf_size = buf_size;
    
    // Allocate send and receive buffers
    conn->send_buf = malloc(buf_size);
    conn->recv_buf = malloc(buf_size);
    if (!conn->send_buf || !conn->recv_buf) {
        fprintf(stderr, "Failed to allocate buffers\n");
        free(conn->send_buf);
        free(conn->recv_buf);
        ibv_destroy_cq(conn->cq);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        return -1;
    }
    
    // Initialize buffers to zero
    memset(conn->send_buf, 0, buf_size);
    memset(conn->recv_buf, 0, buf_size);
    
    // Register memory regions
    int mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    
    conn->send_mr = ibv_reg_mr(conn->pd, conn->send_buf, buf_size, mr_flags);
    conn->recv_mr = ibv_reg_mr(conn->pd, conn->recv_buf, buf_size, mr_flags);
    
    if (!conn->send_mr || !conn->recv_mr) {
        fprintf(stderr, "Failed to register memory regions\n");
        ibv_dereg_mr(conn->send_mr);
        ibv_dereg_mr(conn->recv_mr);
        free(conn->send_buf);
        free(conn->recv_buf);
        ibv_destroy_cq(conn->cq);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        return -1;
    }
    
    // Create queue pair
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = conn->cq,
        .recv_cq = conn->cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr = MAX_WR,
            .max_recv_wr = MAX_WR,
            .max_send_sge = MAX_SGE,
            .max_recv_sge = MAX_SGE
        }
    };
    
    conn->qp = ibv_create_qp(conn->pd, &qp_attr);
    if (!conn->qp) {
        fprintf(stderr, "Failed to create queue pair\n");
        ibv_dereg_mr(conn->send_mr);
        ibv_dereg_mr(conn->recv_mr);
        free(conn->send_buf);
        free(conn->recv_buf);
        ibv_destroy_cq(conn->cq);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        return -1;
    }
    
    // Initialize QP to INIT state
    struct ibv_qp_attr init_attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    };
    
    if (ibv_modify_qp(conn->qp, &init_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | 
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        fprintf(stderr, "Failed to modify QP to INIT state\n");
        ibv_destroy_qp(conn->qp);
        ibv_dereg_mr(conn->send_mr);
        ibv_dereg_mr(conn->recv_mr);
        free(conn->send_buf);
        free(conn->recv_buf);
        ibv_destroy_cq(conn->cq);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        return -1;
    }
    
    return 0;
}

void cleanup_rdma_connection(rdma_connection_t *conn) {
    if (!conn) return;
    
    if (conn->qp) {
        ibv_destroy_qp(conn->qp);
        conn->qp = NULL;
    }
    
    if (conn->send_mr) {
        ibv_dereg_mr(conn->send_mr);
        conn->send_mr = NULL;
    }
    
    if (conn->recv_mr) {
        ibv_dereg_mr(conn->recv_mr);
        conn->recv_mr = NULL;
    }
    
    if (conn->send_buf) {
        free(conn->send_buf);
        conn->send_buf = NULL;
    }
    
    if (conn->recv_buf) {
        free(conn->recv_buf);
        conn->recv_buf = NULL;
    }
    
    if (conn->cq) {
        ibv_destroy_cq(conn->cq);
        conn->cq = NULL;
    }
    
    if (conn->pd) {
        ibv_dealloc_pd(conn->pd);
        conn->pd = NULL;
    }
    
    if (conn->context) {
        ibv_close_device(conn->context);
        conn->context = NULL;
    }
    
    conn->connected = 0;
}

int establish_rdma_connections(PGHandle *pg_handle) {
    if (!pg_handle) {
        return -1;
    }
    
    // Move left connection QP to RTR (Ready to Receive) state
    struct ibv_port_attr port_attr;
    if (ibv_query_port(pg_handle->left_conn.context, 1, &port_attr) != 0) {
        fprintf(stderr, "Failed to query port attributes\n");
        return -1;
    }
    
    struct ibv_qp_attr rtr_attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = 0,  // Will be set from exchanged info
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 0,
            .dlid = 0,     // Will be set from exchanged info
            .sl = 0,
            .src_path_bits = 0,
            .port_num = 1
        }
    };
    
    // Move right connection QP to RTR state (similar setup)
    struct ibv_qp_attr rtr_attr_right = rtr_attr;
    
    // For this skeleton, we'll assume the connections are established
    // In a real implementation, you would use the exchanged rdma_info_t
    // to properly set up the connection parameters
    
    // Move QPs to RTS (Ready to Send) state
    struct ibv_qp_attr rts_attr = {
        .qp_state = IBV_QPS_RTS,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .sq_psn = 0,
        .max_rd_atomic = 1
    };
    
    // Mark connections as established
    pg_handle->left_conn.connected = 1;
    pg_handle->right_conn.connected = 1;
    
    return 0;
}

int rdma_write_data(rdma_connection_t *conn, void *local_buf, size_t size, uint64_t remote_addr) {
    if (!conn || !conn->connected || !local_buf || size == 0) {
        return -1;
    }
    
    // Prepare scatter/gather element
    struct ibv_sge sge = {
        .addr = (uint64_t)local_buf,
        .length = size,
        .lkey = conn->send_mr->lkey
    };
    
    // Prepare work request
    struct ibv_send_wr wr = {
        .wr_id = (uint64_t)local_buf,
        .next = NULL,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = remote_addr,
        .wr.rdma.rkey = conn->remote_rkey
    };
    
    // Post send request
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(conn->qp, &wr, &bad_wr) != 0) {
        fprintf(stderr, "Failed to post RDMA write\n");
        return -1;
    }
    
    // Wait for completion
    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(conn->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "Failed to poll completion queue\n");
            return -1;
        }
    } while (ne == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "RDMA write completed with error: %s\n", 
                ibv_wc_status_str(wc.status));
        return -1;
    }
    
    return 0;
}