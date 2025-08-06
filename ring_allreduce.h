#ifndef RING_ALLREDUCE_H
#define RING_ALLREDUCE_H

#include "pg_handle.h"

// Ring algorithm implementation
int perform_ring_allreduce(void* sendbuf, void* recvbuf, int count, 
                          DATATYPE datatype, OPERATION op, PGHandle* pg_handle);

// Helper functions for operations
void perform_operation(void* a, void* b, int count, DATATYPE datatype, OPERATION op);
size_t get_datatype_size(DATATYPE datatype);
int get_chunk_size(int total_count, int rank, int size);
int get_chunk_offset(int total_count, int rank, int size);

#endif // RING_ALLREDUCE_H