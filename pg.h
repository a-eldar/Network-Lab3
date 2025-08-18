#ifndef PG_H
#define PG_H

#include <infiniband/verbs.h>
#include <stdint.h>

#define PG_MAX_HOSTS 64
#define PG_PORT_BASE 18515

typedef struct pg_conn pg_conn_t;

int connect_process_group(const char *host_list, int myrank, void **pg_handle);
int pg_all_reduce(void *sendbuf, void *recvbuf, int count, int datatype_bytes, int myrank, void *pg_handle);
int pg_close(void *pg_handle);

#endif /* PG_H */