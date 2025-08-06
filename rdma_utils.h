#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include "pg_handle.h"
#include <infiniband/verbs.h>

#define DEFAULT_PORT 18515
#define DEFAULT_IB_PORT 1
#define DEFAULT_MTU IBV_MTU_2048
#define DEFAULT_RX_DEPTH 100
#define DEFAULT_TX_DEPTH 100
#define WC_BATCH 10

enum {
    RECV_WRID = 1,
    SEND_WRID = 2,
};

// Connection destination info
struct connection_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

// Helper functions
uint16_t get_local_lid(struct ibv_context *context, int port);
int get_port_info(struct ibv_context *context, int port, struct ibv_port_attr *attr);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
enum ibv_mtu mtu_to_enum(int mtu);

// RDMA connection functions
int init_neighbor_connection(struct neighbor_connection *conn, struct ibv_device *ib_dev, 
                            int buf_size, int ib_port, int is_sender);
int connect_qp(struct neighbor_connection *conn, int ib_port, int my_psn, 
               struct connection_dest *dest, int gidx);
int post_recv(struct neighbor_connection *conn);
int post_send(struct neighbor_connection *conn, int size);
int wait_for_completion(struct neighbor_connection *conn);
int cleanup_neighbor_connection(struct neighbor_connection *conn);

#endif // RDMA_UTILS_H