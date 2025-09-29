#ifndef PG_CONNECT_H
#define PG_CONNECT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * pg_connect.h
 *
 * Header for process-group RDMA connect code (ring connection + RDMA resources).
 *
 * This header exposes:
 *  - pg_handle_t : the process-group handle storing RDMA objects
 *  - qp_info_t, mr_info_t : small POD structs exchanged over TCP
 *  - DATATYPE, OPERATION enums : basic collective args
 *  - public API: connect_process_group, pg_all_reduce, pg_close
 *
 * Note: The implementation uses the verbs API. Include this header where you need
 * to call connect_process_group() and the other public APIs.
 */

#include <infiniband/verbs.h>
#include <stdint.h>
#include <stddef.h>

/* Default base ports used for QP and MR exchanges over TCP.
 * The C file referenced QP_EXCHANGE_PORT_BASE and MR_EXCHANGE_PORT_BASE but
 * didn't define them in the snippet you gave; define defaults here.
 * Adjust if your environment uses other ports.
 */
#ifndef QP_EXCHANGE_PORT_BASE
#define QP_EXCHANGE_PORT_BASE 18515
#endif

#ifndef MR_EXCHANGE_PORT_BASE
#define MR_EXCHANGE_PORT_BASE 18525
#endif

/* Maximum server name length used in code */
#define PG_MAX_HOSTNAME_LEN 256

/* Small structure describing a QP's public parameters (LID, QPN, PSN).
 * This is sent/received during QP info exchange over TCP.
 */
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

/* Supported datatypes for PG collectives (expand as needed) */
typedef enum {
    INT,
    DOUBLE
} DATATYPE;

/* Supported reduction operations */
typedef enum {
    SUM,
    MULT
} OPERATION;

/*
 * Process-group handle: holds all verbs objects and bookkeeping fields.
 *
 * Fields are intentionally similar to the source file you posted. Callers
 * should treat this as an opaque handle for most operations — but the fields
 * are exposed for debugging/inspection.
 */
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

/* Public API
 *
 * connect_process_group:
 *   servername: comma-separated list of hostnames or IPs (e.g. "host1,host2,host3")
 *   pg_handle: pointer to a void* that will be set to a newly allocated pg_handle_t*
 *
 * Returns 0 on success, -1 on failure.
 */
int connect_process_group(char *servername, void **pg_handle);

/*
 * pg_all_reduce:
 *   A collective all-reduce implemented using the RDMA resources in pg_handle.
 *   The exact semantics (pipelining, fragmentation) are implemented in the C file.
 *
 *   sendbuf: pointer to local input buffer (count elements of 'datatype')
 *   recvbuf: pointer to output buffer of length 'count'
 *   count:   number of elements
 *   datatype: DATATYPE describing element size
 *   op: operation to apply
 *   pg_handle: pointer to the handle returned from connect_process_group
 *
 * Returns 0 on success, -1 on failure.
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op, void *pg_handle);

/*
 * pg_close:
 *   Destroys QPs, deregisters MRs, closes device, frees handle memory.
 *
 * Returns 0 on success, -1 on failure.
 */
int pg_close(void *pg_handle);


/* Extern declarations for small TCP helper functions that the implementation
 * used. If these are implemented elsewhere, keep the declarations; otherwise,
 * remove or change them to match your TCP helper API.
 */
extern int tcp_connect(const char *host, int port);
extern int tcp_listen_accept(int port);

#ifdef __cplusplus
}
#endif

#endif /* PG_CONNECT_H */
