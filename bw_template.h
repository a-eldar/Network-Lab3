#ifndef BW_TEMPLATE_H
#define BW_TEMPLATE_H


#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include <infiniband/verbs.h>

#define WC_BATCH (10)

// Additional Definitions:
#define BITS_IN_BYTE (8)
#define MEGA (1024 * 1024)


enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

static int page_size;

// The most useful struct for us - used to pass information
struct pingpong_context {
    struct ibv_context		*context;
    struct ibv_comp_channel	*channel;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;
    int				size;
    int				rx_depth;
    int				routs;
    struct ibv_port_attr	portinfo;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};



// Function declarations
enum ibv_mtu pp_mtu_to_enum(int mtu);
uint16_t pp_get_local_lid(struct ibv_context *context, int port);
int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
int pp_post_recv(struct pingpong_context *ctx, int n);
int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx);
int pp_post_send(struct pingpong_context *ctx);
int pp_close_ctx(struct pingpong_context *ctx);

// Helper functions
double calculate_throughput(size_t bytes, double seconds);
void print_throughput(size_t size, double throughput);
void client_send_operation(int max_size, int size_step, int iters, struct pingpong_context *ctx, int tx_depth);
void server_recv_operation(struct pingpong_context *ctx, int iters, int max_size, int size_step);

int bw(char* servername, struct pingpong_context *ctx);


#endif // BW_TEMPLATE_H
