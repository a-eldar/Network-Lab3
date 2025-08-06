#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

uint16_t get_local_lid(struct ibv_context *context, int port) {
    struct ibv_port_attr attr;
    if (ibv_query_port(context, port, &attr))
        return 0;
    return attr.lid;
}

int get_port_info(struct ibv_context *context, int port, struct ibv_port_attr *attr) {
    return ibv_query_port(context, port, attr);
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid) {
    char tmp[9];
    uint32_t v32;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
    }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    int i;
    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

enum ibv_mtu mtu_to_enum(int mtu) {
    switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return -1;
    }
}

int init_neighbor_connection(struct neighbor_connection *conn, struct ibv_device *ib_dev, 
                            int buf_size, int ib_port, int is_sender) {
    memset(conn, 0, sizeof(*conn));
    
    conn->buf_size = buf_size;
    
    // Allocate buffer
    int page_size = sysconf(_SC_PAGESIZE);
    conn->buf = malloc((buf_size + page_size - 1) & ~(page_size - 1));
    if (!conn->buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }
    memset(conn->buf, 0, buf_size);
    
    // Open device
    conn->context = ibv_open_device(ib_dev);
    if (!conn->context) {
        fprintf(stderr, "Failed to open device\n");
        free(conn->buf);
        return -1;
    }
    
    // Allocate PD
    conn->pd = ibv_alloc_pd(conn->context);
    if (!conn->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        ibv_close_device(conn->context);
        free(conn->buf);
        return -1;
    }
    
    // Register memory
    conn->mr = ibv_reg_mr(conn->pd, conn->buf, buf_size, IBV_ACCESS_LOCAL_WRITE |
                         IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!conn->mr) {
        fprintf(stderr, "Failed to register MR\n");
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        free(conn->buf);
        return -1;
    }
    
    // Create CQ
    conn->cq = ibv_create_cq(conn->context, DEFAULT_RX_DEPTH + DEFAULT_TX_DEPTH, 
                            NULL, NULL, 0);
    if (!conn->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        ibv_dereg_mr(conn->mr);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        free(conn->buf);
        return -1;
    }
    
    // Create QP
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = conn->cq,
        .recv_cq = conn->cq,
        .cap = {
            .max_send_wr = DEFAULT_TX_DEPTH,
            .max_recv_wr = DEFAULT_RX_DEPTH,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    
    conn->qp = ibv_create_qp(conn->pd, &qp_attr);
    if (!conn->qp) {
        fprintf(stderr, "Failed to create QP\n");
        ibv_destroy_cq(conn->cq);
        ibv_dereg_mr(conn->mr);
        ibv_dealloc_pd(conn->pd);
        ibv_close_device(conn->context);
        free(conn->buf);
        return -1;
    }
    
    // Modify QP to INIT
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = ib_port,
        .qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    };
    
    if (ibv_modify_qp(conn->qp, &attr,
                     IBV_QP_STATE | IBV_QP_PKEY_INDEX | 
                     IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        cleanup_neighbor_connection(conn);
        return -1;
    }
    
    return 0;
}

int connect_qp(struct neighbor_connection *conn, int ib_port, int my_psn, 
               struct connection_dest *dest, int gidx) {
    
    // Modify QP to RTR
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = DEFAULT_MTU,
        .dest_qp_num = dest->qpn,
        .rq_psn = dest->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 0,
            .dlid = dest->lid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = ib_port
        }
    };
    
    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = gidx;
    }
    
    if (ibv_modify_qp(conn->qp, &attr,
                     IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                     IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                     IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return -1;
    }
    
    // Modify QP to RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_psn;
    attr.max_rd_atomic = 1;
    
    if (ibv_modify_qp(conn->qp, &attr,
                     IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                     IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return -1;
    }
    
    return 0;
}

int post_recv(struct neighbor_connection *conn) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)conn->buf,
        .length = conn->buf_size,
        .lkey = conn->mr->lkey
    };
    
    struct ibv_recv_wr wr = {
        .wr_id = RECV_WRID,
        .sg_list = &sge,
        .num_sge = 1,
        .next = NULL
    };
    
    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(conn->qp, &wr, &bad_wr);
}

int post_send(struct neighbor_connection *conn, int size) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)conn->buf,
        .length = size,
        .lkey = conn->mr->lkey
    };
    
    struct ibv_send_wr wr = {
        .wr_id = SEND_WRID,
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .next = NULL
    };
    
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(conn->qp, &wr, &bad_wr);
}

int wait_for_completion(struct neighbor_connection *conn, int expected_completions) {
    int completed = 0;
    
    while (completed < expected_completions) {
        struct ibv_wc wc[WC_BATCH];
        int ne, i;
        
        do {
            ne = ibv_poll_cq(conn->cq, WC_BATCH, wc);
            if (ne < 0) {
                fprintf(stderr, "Failed to poll CQ\n");
                return -1;
            }
        } while (ne < 1);
        
        for (i = 0; i < ne; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Work completion failed: %s\n", 
                       ibv_wc_status_str(wc[i].status));
                return -1;
            }
            completed++;
        }
    }
    
    return 0;
}

int cleanup_neighbor_connection(struct neighbor_connection *conn) {
    int ret = 0;
    
    if (conn->qp && ibv_destroy_qp(conn->qp)) {
        fprintf(stderr, "Failed to destroy QP\n");
        ret = -1;
    }
    
    if (conn->cq && ibv_destroy_cq(conn->cq)) {
        fprintf(stderr, "Failed to destroy CQ\n");
        ret = -1;
    }
    
    if (conn->mr && ibv_dereg_mr(conn->mr)) {
        fprintf(stderr, "Failed to deregister MR\n");
        ret = -1;
    }
    
    if (conn->pd && ibv_dealloc_pd(conn->pd)) {
        fprintf(stderr, "Failed to deallocate PD\n");
        ret = -1;
    }
    
    if (conn->context && ibv_close_device(conn->context)) {
        fprintf(stderr, "Failed to close device\n");
        ret = -1;
    }
    
    if (conn->buf) {
        free(conn->buf);
        conn->buf = NULL;
    }
    
    return ret;
}