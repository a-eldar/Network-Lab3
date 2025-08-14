#ifndef PG_HANDLE_H
#define PG_HANDLE_H

#include <infiniband/verbs.h>
#include <stdint.h>

#define MAX_WR_ID 1000
#define RDMA_BUFFER_SIZE (1024 * 1024 * 16)  // 16MB buffer for RDMA operations

typedef enum {
    INT,
    DOUBLE
} DATATYPE;

typedef enum {
    SUM,
    MULT
} OPERATION;

struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;
    int				size;
    int				rx_depth;
    int				routs;
    struct ibv_port_attr	portinfo;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

// Main handle structure for process group
typedef struct PGHandle {
    // Server information
    int num_servers;
    int server_idx;
    char **server_list;
    
    // RDMA connections
    // left_conn: Buffer for writing data that left neighbor will read from us
    // right_conn: Connection for reading data from left neighbor
    struct pingpong_context *left_conn;
    struct pingpong_context *right_conn;
    
    // Synchronization
    int connected;
    
} PGHandle;


#endif // PG_HANDLE_H