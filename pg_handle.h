#ifndef PG_HANDLE_H
#define PG_HANDLE_H

#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "bw_template.h"

#define MAX_WR_ID 1000
#define RDMA_BUFFER_SIZE (1024 * 1024 * 16)  // 16MB buffer for RDMA operations

typedef enum {
    INT,
    DOUBLE
} DATATYPE;

typedef enum {
    SUM,
    MULT
} OPERATION;



// Main handle structure for process group
typedef struct PGHandle {
    // Server information
    int num_servers;
    int server_idx;
    char **server_list;
    
    // RDMA connections
    // left_conn: Buffer for writing data that left neighbor will read from us
    // right_conn: Connection for reading data from left neighbor
    struct pingpong_context *left_conn;
    struct pingpong_context *right_conn;
    
    // Synchronization
    int connected;
    
} PGHandle;



#endif // PG_HANDLE_H