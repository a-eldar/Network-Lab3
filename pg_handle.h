// RDMA Ring Collectives Library - Handle and public API
// This file declares the main handle, enums, and public functions.

#ifndef PG_HANDLE_H
#define PG_HANDLE_H

#include <infiniband/verbs.h>
#include <stdint.h>
#include <stddef.h>

#ifndef DEBUG
#define DEBUG 1
#endif

typedef enum { INT = 0, DOUBLE = 1 } DATATYPE;
typedef enum { SUM = 0, MULT = 1 } OPERATION;

#define PG_MAX_HOSTNAME_LEN 256
#define PG_DEFAULT_TCP_PORT 18515
#define PG_DEFAULT_IB_PORT 1
#define PG_DEFAULT_CQ_DEPTH 1024
#define PG_DEFAULT_QP_DEPTH 512
#define PG_DEFAULT_MTU IBV_MTU_1024
#define PG_MAX_MESSAGE_BYTES (4 * 1024 * 1024)

typedef struct PgRemoteInfo {
	uint32_t qp_num;
	uint16_t lid;
	uint8_t gid[16];
	int has_gid;
	uint32_t rkey;
	uint64_t buf_addr; // base address of peer receive buffer
} PgRemoteInfo;

typedef struct {
	// RDMA device resources
	struct ibv_context* context;
	struct ibv_pd* pd;
	struct ibv_cq* cq;

	// Two reliable connected QPs: to left and to right neighbor
	struct ibv_qp* qp_left;
	struct ibv_qp* qp_right;

	// Registered memory regions for send/recv with left/right
	void* sendbuf_left;
	void* recvbuf_left;
	struct ibv_mr* mr_send_left;
	struct ibv_mr* mr_recv_left;

	void* sendbuf_right;
	void* recvbuf_right;
	struct ibv_mr* mr_send_right;
	struct ibv_mr* mr_recv_right;

	// Per-neighbor remote connection info
	PgRemoteInfo remote_left;
	PgRemoteInfo remote_right;

	// Group topology
	int world_size;
	int my_rank;
	int left_rank;
	int right_rank;

	// IB port and attributes
	uint8_t ib_port;
	int is_roce;

	// Messaging configuration
	size_t max_message_bytes;
} PGHandle;

// Connection management
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
int pg_close(PGHandle* pg_handle);

// Collectives
int pg_reduce_scatter(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
int pg_all_gather(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, PGHandle* pg_handle);
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);

#endif // PG_HANDLE_H

