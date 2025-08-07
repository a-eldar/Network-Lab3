#ifndef PG_ALLREDUCE_H
#define PG_ALLREDUCE_H

#include "pg_handle.h"

// Connect to process group
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);

// Perform all-reduce operation
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);

// Close process group connection
int pg_close(PGHandle* pg_handle);

#endif // PG_ALLREDUCE_H