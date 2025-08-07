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

// Structure to hold RDMA connection information
typedef struct {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;
    
    // Buffer for RDMA operations
    void *buf;
    size_t buf_size;
    
    // Remote connection info
    uint32_t remote_qpn;
    uint16_t remote_lid;
    uint32_t remote_psn;
    uint64_t remote_addr;
    uint32_t remote_rkey;
} RDMAConnection;

// Main handle structure for process group
typedef struct PGHandle {
    // Server information
    int num_servers;
    int server_idx;
    char **server_list;
    
    // RDMA connections
    RDMAConnection *left_conn;   // Connection to read from left neighbor
    RDMAConnection *right_conn;  // Connection to write to right neighbor
    
    // Synchronization
    int connected;
    
    // Ring algorithm specific
    void *work_buffer;  // Working buffer for ring operations
    size_t work_buffer_size;
    
} PGHandle;

// Connection info structure for TCP exchange
typedef struct {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
    uint64_t addr;
    uint32_t rkey;
} ConnectionInfo;

#endif // PG_HANDLE_H