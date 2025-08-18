#ifndef PG_ALLREDUCE_H
#define PG_ALLREDUCE_H

#include "pg_handle.h"

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
int pg_reduce_scatter(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
int pg_all_gather(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, PGHandle* pg_handle);
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
int pg_close(PGHandle* pg_handle);

#endif // PG_ALLREDUCE_H

