#ifndef PG_HANDLE_H
#define PG_HANDLE_H

#include <infiniband/verbs.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

typedef enum {
    INT, DOUBLE
} DATATYPE;

typedef enum {
    SUM, MULT
} OPERATION;

// Connection information for a single neighbor
struct neighbor_connection {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    void *buf;
    int buf_size;
    
    // Remote connection info
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};

// Main handle structure for the process group
typedef struct {
    int rank;                    // My index in the process group
    int size;                    // Total number of processes
    char **serverlist;           // Copy of server names
    
    // RDMA connections to neighbors
    struct neighbor_connection left_neighbor;   // Receive from left
    struct neighbor_connection right_neighbor;  // Send to right
    
    // IB device info
    struct ibv_device *ib_dev;
    int ib_port;
    int page_size;
    
    // Synchronization
    int max_buffer_size;
} PGHandle;

// Function declarations
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
int pg_close(PGHandle* pg_handle);

#endif // PG_HANDLE_H