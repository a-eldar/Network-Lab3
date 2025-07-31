#ifndef API_H
#define API_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <infiniband/verbs.h>
#include <stdint.h>

// Enums for data types and operations
typedef enum {
    INT,
    DOUBLE
} DATATYPE;

typedef enum {
    SUM,
    MULT
} OPERATION;

// Structure to hold RDMA connection information for each neighbor
typedef struct {
    struct ibv_qp *qp;              // Queue Pair for RDMA operations
    struct ibv_mr *local_mr;        // Memory Region for local buffer
    struct ibv_mr *remote_mr;       // Memory Region information from remote
    uint32_t remote_qpn;            // Remote Queue Pair Number
    uint16_t remote_lid;            // Remote Local Identifier
    union ibv_gid remote_gid;       // Remote Global Identifier
    uint64_t remote_addr;           // Remote memory address
    uint32_t remote_rkey;           // Remote key for memory access
    void *local_buffer;             // Local buffer for data
    size_t buffer_size;             // Size of the buffer
} rdma_connection_t;

// Main process group handle structure
typedef struct {
    // Basic ring topology information
    int rank;                       // My rank in the ring (0 to num_processes-1)
    int num_processes;              // Total number of processes in the ring
    char **server_names;            // Array of server names
    char *my_hostname;              // My hostname
    
    // RDMA device and context
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_context *context;
    struct ibv_pd *pd;              // Protection Domain
    struct ibv_cq *cq;              // Completion Queue
    
    // Ring connections (left and right neighbors)
    rdma_connection_t left_neighbor;   // Connection to left neighbor in ring
    rdma_connection_t right_neighbor;  // Connection to right neighbor in ring
    
    // Working buffers for operations
    void *work_buffer;              // Working buffer for intermediate results
    size_t work_buffer_size;        // Size of working buffer
    
    // Synchronization and state
    int initialized;                // Flag to check if properly initialized
    
    // For pipelining (advanced feature)
    size_t chunk_size;              // Size of chunks for pipelined operations
    int pipelining_enabled;         // Flag for pipelining feature
    
} pg_handle;

// Buffer size constant (as referenced in the example)
#define BUFFER_SIZE (4 * 1024 * 1024)  // 4MB default buffer size

// Function prototypes
int connect_process_group(char *servername, void **pg_handle);
int pg_close(void *pg_handle);
int pg_all_reduce(void *sendbuf, void *recvbuf, int count, 
                  DATATYPE datatype, OPERATION op, void *pg_handle);
int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count, 
                      DATATYPE datatype, OPERATION op, void *pg_handle);
int pg_all_gather(void *sendbuf, void *recvbuf, int count, 
                  DATATYPE datatype, OPERATION op, void *pg_handle);

// Helper function prototypes
static int setup_rdma_resources(pg_handle *handle);
static int establish_tcp_connections(pg_handle *handle, char *server_list);
static int exchange_rdma_info(pg_handle *handle);
static int create_qp_connections(pg_handle *handle);
static void cleanup_rdma_resources(pg_handle *handle);




#endif