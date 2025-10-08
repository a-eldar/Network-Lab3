#ifndef PG_ALLREDUCE_H
#define PG_ALLREDUCE_H

#include "pg_handle.h"

/**
 * @brief Perform an all-reduce operation across the process group.
 * @param sendbuf Pointer to the local input buffer (count elements of 'datatype').
 * @param recvbuf Pointer to the output buffer of length 'count'.
 * @param count Number of elements in sendbuf and recvbuf.
 * @param datatype DATATYPE describing the element type (e.g., INT, DOUBLE).
 * @param op OPERATION to apply (e.g., SUM, MULT).
 * @param pg_handle Pointer to the process group handle returned from connect_process_group.
 * @return 0 on success, -1 on failure.
 */
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);



#endif // PG_ALLREDUCE_H