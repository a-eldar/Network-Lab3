#ifndef PG_H
#define PG_H

#include <infiniband/verbs.h>
#include <stdint.h>

#define PG_MAX_HOSTS 64
#define PG_PORT_BASE 18515

/* opaque handle */
typedef struct pg_conn pg_conn_t;

/* Connect the process group
 * host_list: space-separated list of hostnames or IPs (e.g. "host1 host2 host3")
 * myrank: 0-based rank of this process in the list
 * out: pointer to pg_conn_t* to receive handle
 *
 * Returns 0 on success.
 */
int connect_process_group(const char *host_list, int myrank, void **pg_handle);

/* All reduce: sum of int32 values.
 * sendbuf and recvbuf must point to arrays of 'count' elements (each element
 * has datatype_bytes bytes, but current implementation assumes 4 (int32)).
 * datatype_bytes: size of element (use 4)
 * myrank: rank of this process (required here to compute chunk)
 */
int pg_all_reduce(void *sendbuf, void *recvbuf, int count, int datatype_bytes, int myrank, void *pg_handle);

/* Close and free resources */
int pg_close(void *pg_handle);

#endif /* PG_H */
