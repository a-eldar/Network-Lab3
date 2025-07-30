#ifndef API_H
#define API_H

#include <stddef.h>  // For size_t
// need ibv.h for ibv_context, ibv_pd, ibv_cq, ibv_qp, ibv_mr
#include <infiniband/verbs.h>  // Include the Infiniband verbs

typedef enum {
    INTEGER, DOUBLE
} DATATYPE;

typedef enum {
    SUM, MULT
} OPERATION;

typedef struct {
    int index;
    int num_users;
    struct ibv_context *context;
    struct ibv_pd *pd;
} PGHandle;

////////////// FUNCTIONS //////////////




#endif