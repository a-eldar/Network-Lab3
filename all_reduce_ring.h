#ifndef ALL_REDUCE_RING_H
#define ALL_REDUCE_RING_H

#include "rdma_utils.h"

// #define DEFAULT_COUNT 1024
#define DEFAULT_COUNT 1073741824
#define DEFAULT_DATATYPE INT
#define DEFAULT_OPERATION MEAN

typedef enum OPERATION {
    SUM,
    MIN,
    MAX,
    MEAN
} OPERATION;


int get_default_data(void** data, DATATYPE* datatype, int* count, OPERATION* op, int ring_location);
void test_default_data_after_procedure(void* data, const int count, const int ring_size, const DATATYPE datatype);
void release_data(void** data);

void reduce(void* vec_a, void* vec_b, const int chunk_size, const DATATYPE datatype, const OPERATION op);
int pg_all_reduce(void *sendbuf, void *recvbuf, const int count, const DATATYPE datatype, const OPERATION op, void *pg_handle);
int pg_reduce_scatter(void *sendbuf, void *recvbuf, const int count, const DATATYPE datatype, const OPERATION op, void *pg_handle);
int pg_all_gather(void *sendbuf, const int count, const DATATYPE datatype, void *pg_handle);


#endif  // ALL_REDUCE_RING_H