#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

#include "pg_handle.h"
#include <netdb.h>

// RDMA initialization and setup
int init_rdma_connection(RDMAConnection *conn);
int setup_rdma_resources(RDMAConnection *conn);
int modify_qp_to_init(struct ibv_qp *qp);
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid, uint32_t remote_psn);
int modify_qp_to_rts(struct ibv_qp *qp);
void cleanup_rdma_connection(RDMAConnection *conn);

// RDMA operations
int post_rdma_write(RDMAConnection *conn, void *local_addr, size_t size, uint64_t remote_addr, uint32_t remote_rkey);
int post_rdma_read(RDMAConnection *conn, void *local_addr, size_t size, uint64_t remote_addr, uint32_t remote_rkey);
int wait_for_completion(RDMAConnection *conn);

// TCP exchange utilities
int exchange_connection_info_as_sender(const char *hostname, int port, ConnectionInfo *local_info, ConnectionInfo *remote_info);
int exchange_connection_info_as_receiver(int port, ConnectionInfo *local_info, ConnectionInfo *remote_info);

// Helper functions
int get_server_ip(const char *hostname, char *ip_str, size_t ip_str_size);

#endif // RDMA_UTILS_H