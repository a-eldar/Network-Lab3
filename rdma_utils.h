#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#define _GNU_SOURCE
#include <stdbool.h>
#include <infiniband/verbs.h>  // for RDMA Verbs API

#define DEBUG_MODE true

#define TCP_PORT 11397
#define IB_PORT  1

struct pg_dest {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
};

struct pg_side {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct pg_dest self_dest;
    struct pg_dest* other_dest;
};

struct write_credentials {
    uint64_t recvbuf_addr;
    uint32_t recvbuf_rkey;
    uint64_t sendbuf_addr;
    uint32_t sendbuf_rkey;
};

struct pg_handle_t {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    struct pg_side front;
    struct pg_side back;
    struct write_credentials front_credentials;
    int ring_location;  // p in our scheme
    int ring_size;  // n in our scheme
    struct ibv_port_attr port_attr;
};

typedef enum DATATYPE {
    INT,
    FLOAT,
    DOUBLE
} DATATYPE;

/**
 * connect_process_group - Connects the processes group to the servername and initializes the process group handle.
 * @param servername: The name of the server to connect to.
 * @param pg_handle: The pointer to the process group handle to initialize.
 * @param ring_size: The number of processes in the group.
 * @param ring_location: The location of the current process in the ring.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int connect_process_group(char *servername, void **pg_handle, const uint8_t ring_size, const uint8_t ring_location); /* Connect processes group */

/**
 * pg_close - Destroys the QPs and the context of the processes group.
 * @param pg_handle: The pointer to the process group handle to destroy.
 */
int pg_close(void *pg_handle); /* Destroys the QPs and the context of the processes group */

int register_memory(void* data, DATATYPE datatype, int count, struct pg_handle_t* pg_handle, void** sendbuf, void** recvbuf);
int exchange_registered_memory(struct pg_handle_t* pg_handle);
int unregister_memory(struct pg_handle_t* pg_handle, void** sendbuf, void** recvbuf);

size_t get_datatype_size(DATATYPE datatype);

#endif  // RDMA_UTILS_H