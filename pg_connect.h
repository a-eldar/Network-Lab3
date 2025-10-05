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

#include "pg_handle.h"
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

#ifdef __cplusplus
}
#endif

#endif /* PG_CONNECT_H */
