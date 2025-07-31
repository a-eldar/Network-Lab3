#ifndef API_H
#define API_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <infiniband/verbs.h>
#include <stdint.h>


typedef struct rdma_peer {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buffer;
    size_t buffer_size;
    int lid;
    int qpn;
    int psn;
    int peer_lid;
    int peer_qpn;
    int peer_psn;
    // Additional metadata if needed
} rdma_peer_t;

typedef struct pg_handle {
    int num_servers;
    int my_index;

    // Hostnames of all servers in the group
    char **server_names;

    // Connections to the two neighbors in the ring
    rdma_peer_t left_peer;   // Previous server (index - 1)
    rdma_peer_t right_peer;  // Next server (index + 1)

    // Any common RDMA context if shared (optional)
    struct ibv_device **device_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
} pg_handle_t;

// Enums for data types and operations
typedef enum {
    INT,
    DOUBLE
} DATATYPE;

typedef enum {
    SUM,
    MULT
} OPERATION;




#endif