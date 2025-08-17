#ifndef RDMA_UTILS_H
#define RDMA_UTILS_H

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
#include "pg_handle.h"

#define WC_BATCH (10)

// Additional Definitions:
#define BITS_IN_BYTE (8)
#define MEGA (1024 * 1024)


enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};



// The most useful struct for us - used to pass information


// enum ibv_mtu pp_mtu_to_enum(int mtu)
// {
//     switch (mtu) {
//     case 256:  return IBV_MTU_256;
//     case 512:  return IBV_MTU_512;
//     case 1024: return IBV_MTU_1024;
//     case 2048: return IBV_MTU_2048;
//     case 4096: return IBV_MTU_4096;
//     default:   return -1;
//     }
// }

int pp_get_port_info(struct ibv_context *context, int port,
                     struct ibv_port_attr *attr);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
                          enum ibv_mtu mtu, int sl,
                          struct pingpong_dest *dest, int sgid_idx);

                        
struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
                                                 const struct pingpong_dest *my_dest);

struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
                                                 int ib_port, enum ibv_mtu mtu,
                                                 int port, int sl,
                                                 const struct pingpong_dest *my_dest,
                                                 int sgid_idx);

struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int tx_depth, int port,
                                            int use_event, int is_server);

int pp_close_ctx(struct pingpong_context *ctx);

int pp_post_recv(struct pingpong_context *ctx, int n);

int pp_post_send(struct pingpong_context *ctx);

int pp_wait_completions(struct pingpong_context *ctx, int iters);



#endif // RDMA_UTILS_H