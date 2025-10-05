#ifndef PG_HANDLE_H
#define PG_HANDLE_H

#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

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

typedef struct {
    uint16_t lid;
    uint32_t qpn;
    uint32_t psn;
} qp_info_t;

/* Memory region info exchanged with neighbors (rkey + address) */
typedef struct {
    uint32_t rkey;
    uintptr_t addr;
} mr_info_t;

typedef struct addrinfo{
    int ai_flags;              /* Input flags.  */
    int ai_family;             /* Protocol family for socket.  */
    int ai_socktype;           /* Socket type.  */
    int ai_protocol;           /* Protocol for socket.  */
    size_t ai_addrlen;         /* Length of socket address.  */
    struct sockaddr *ai_addr;  /* Socket address for socket.  */
    char *ai_canonname;        /* Canonical name for service location.  */
    struct addrinfo *ai_next;  /* Pointer to next in list.  */
    struct in_addr sin_addr;
}

typedef struct pg_handle {
    /* process group identity */
    int rank;          /* local rank in the provided server list */
    int size;          /* total number of ranks */

    /* server names parsed from the server list (array of size 'size') */
    char **servernames; /* owned by handle; freed during pg_close */

    /* RDMA device / protection domain / CQs / QPs */
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp **qps; /* array of 2 QPs: [0] = left, [1] = right */

    /* Memory regions and buffers */
    void *sendbuf;        /* local send buffer (registered) */
    void *recvbuf;        /* local recv buffer (registered) */
    struct ibv_mr *mr_send;
    struct ibv_mr *mr_recv;

    /* local memory info (for sharing if necessary) */
    uint32_t local_rkey;
    uintptr_t local_addr;
    size_t bufsize;       /* size of send/recv buffers */

    /* remote neighbors' info mapped by rank index */
    uint32_t *remote_rkeys;   /* array size 'size' */
    uintptr_t *remote_addrs;  /* array size 'size' */

    /* optional extras that might be useful to keep */
    /* page size or other config values could be added here */
} pg_handle_t;



#endif // PG_HANDLE_H