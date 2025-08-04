// rdma_allreduce.h
#ifndef RDMA_ALLREDUCE_H
#define RDMA_ALLREDUCE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

// Data types and operations
typedef enum {
    INT, DOUBLE
} DATATYPE;

typedef enum {
    SUM, MULT
} OPERATION;

// RDMA connection structure for a single neighbor
typedef struct {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    void *send_buf;
    void *recv_buf;
    size_t buf_size;
    uint32_t remote_rkey;
    uint64_t remote_addr;
    int connected;
} rdma_connection_t;

// Process Group Handle - holds all communication data
typedef struct {
    int num_processes;      // Total number of processes in the ring
    int my_rank;           // My position in the ring (0 to num_processes-1)
    
    // RDMA connections
    rdma_connection_t left_conn;   // Connection to read from (left neighbor)
    rdma_connection_t right_conn;  // Connection to write to (right neighbor)
    
    // TCP sockets for initial setup
    int tcp_listen_fd;
    int tcp_client_fd;
    
    // Buffer management
    void *work_buffer;     // Working buffer for ring operations
    size_t buffer_size;    // Size of working buffer
    
    // Ring algorithm state
    int ring_initialized;
} PGHandle;

// Helper structure for TCP communication during setup
typedef struct {
    uint32_t rkey;
    uint64_t addr;
    union ibv_gid gid;
    uint16_t lid;
    uint32_t qp_num;
} rdma_info_t;

// Main API Functions
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
int pg_close(PGHandle* pg_handle);

// RDMA helper functions (declared for internal use)
int setup_rdma_connection(rdma_connection_t *conn, size_t buf_size);
void cleanup_rdma_connection(rdma_connection_t *conn);
int establish_rdma_connections(PGHandle *pg_handle);

// TCP helper functions
int setup_tcp_server(int port);
int connect_tcp_client(const char *hostname, int port);
int exchange_rdma_info(PGHandle *pg_handle, char **serverlist, int len, int idx);

// Ring algorithm helpers
int perform_ring_reduce_scatter(void *data, int count, DATATYPE datatype, OPERATION op, PGHandle *pg_handle);
int perform_ring_allgather(void *data, int count, DATATYPE datatype, PGHandle *pg_handle);

// Utility functions
void apply_operation(void *dest, void *src, int count, DATATYPE datatype, OPERATION op);
size_t get_datatype_size(DATATYPE datatype);
char* resolve_hostname(const char *hostname);

// Constants
#define DEFAULT_TCP_PORT 12345
#define DEFAULT_BUFFER_SIZE (1024 * 1024)  // 1MB
#define MAX_WR 16
#define MAX_SGE 1

#endif // RDMA_ALLREDUCE_H