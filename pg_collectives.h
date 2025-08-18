// Collective operations over RDMA ring
#ifndef PG_COLLECTIVES_H
#define PG_COLLECTIVES_H

#include "pg_handle.h"

size_t datatype_sizeof(DATATYPE dtype);
void apply_op(void* dst, const void* src, int count, DATATYPE dtype, OPERATION op);

#endif // PG_COLLECTIVES_H

