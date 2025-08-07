#ifndef PG_HANDLE_H
#define PG_HANDLE_H
#include <stdint.h>
#include <infiniband/verbs.h>

struct destination_info {
    int lid;                // Local Identifier
    int qpn;               // Queue Pair Number
    int psn;               // Packet Sequence Number
    union ibv_gid gid;     // Global Identifier
};



struct neighbor_connection {
    //queue pair
    struct ibv_qp *qp;
    //completion queue
    struct ibv_cq *cq;
    struct destination_info dest; // Destination info for connection
    struct ibv_mr *mr; // Memory region for RDMA operations
    void* buf; // Buffer for RDMA operations
    size_t buf_size; // Size of the buffer
    
};


struct pg_handle
{
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct neighbor_connection left_neighbor; // Connection to the left neighbor
    struct neighbor_connection right_neighbor; // Connection to the right neighbor
    int rank; // Rank of the process in the group
    int size; // Size of the group
    struct ibv_port_attr port_attr; // Port attributes
};
