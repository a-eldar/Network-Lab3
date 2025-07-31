#ifndef CONNECTION_H
#define CONNECTION_H
#include "api.h"

#define TCP_PORT_BASE 12345
#define MAX_RETRIES 100
#define RETRY_DELAY_MS 100

// Structure to exchange RDMA connection info via TCP
typedef struct {
    uint32_t qpn;           // Queue Pair Number
    uint16_t lid;           // Local Identifier
    union ibv_gid gid;      // Global Identifier
    uint64_t addr;          // Memory address
    uint32_t rkey;          // Remote key
} rdma_conn_info_t;

// Helper function prototypes
int connect_process_group(char *server_list, void **pg_handle_ptr)
static int parse_server_list(const char *server_list, char ***servers, int *count);
static int find_my_rank(char **servers, int count, const char *hostname);
static int setup_rdma_device(pg_handle *handle);
static int create_memory_regions(pg_handle *handle);
static int create_queue_pairs(pg_handle *handle);
static int establish_tcp_connections(pg_handle *handle);
static int exchange_rdma_info_tcp(pg_handle *handle);
static int connect_qps(pg_handle *handle);
static void cleanup_on_error(pg_handle *handle);

#endif // CONNECTION_H