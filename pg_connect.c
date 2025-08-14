
#include "pg_connect.h"

static int page_size;


static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int tx_depth, int port,
                                            int use_event, int is_server)
{
    struct pingpong_context *ctx;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size     = size;
    ctx->rx_depth = rx_depth;
    ctx->routs    = rx_depth;
    int page_size = sysconf(_SC_PAGESIZE);
    ctx->buf = malloc(roundup(size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }

    memset(ctx->buf, 0x02 + is_server, size); // server: 0x03, client: 0x02

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }

    if (use_event) {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            fprintf(stderr, "Couldn't create completion channel\n");
            return NULL;
        }
    } else
        ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }

    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
            ctx->channel, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    {
        struct ibv_qp_init_attr attr = {
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .cap     = {
                        .max_send_wr  = tx_depth,
                        .max_recv_wr  = rx_depth,
                        .max_send_sge = 1,
                        .max_recv_sge = 1
                },
                .qp_type = IBV_QPT_RC
        };

        ctx->qp = ibv_create_qp(ctx->pd, &attr);
        if (!ctx->qp)  {
            fprintf(stderr, "Couldn't create QP\n");
            return NULL;
        }
    }

    {
        struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = port,
                .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(ctx->qp, &attr,
                IBV_QP_STATE              |
                IBV_QP_PKEY_INDEX         |
                IBV_QP_PORT               |
                IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
    }

    return ctx;
}


int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle){
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct pingpong_dest     my_dest_left, my_dest_right;
    struct pingpong_dest    *rem_dest_left, *rem_dest_right;
    char                    *ib_devname = NULL;
    char                    *servername = NULL; // Modified: added `= NULL`
    int                      port = 12345;
    int                      ib_port = 1;
    enum ibv_mtu             mtu = IBV_MTU_2048;
    if (!serverlist || len <= 0 || idx < 0 || idx >= len || !pg_handle) {
        fprintf(stderr, "Invalid parameters for connect_process_group\n");
        return -1;
    }

    pg_handle->num_servers = len;
    pg_handle->server_idx = idx;
    pg_handle->server_list = malloc(len * sizeof(char*));
    if (!pg_handle->server_list) {
        fprintf(stderr, "Memory allocation failed for server list\n");
        return -1;
    }
    for (int i = 0; i < len; i++) {
        pg_handle->server_list[i] = strdup(serverlist[i]);
        if (!pg_handle->server_list[i]) {
            fprintf(stderr, "Memory allocation failed for server name\n");
            for (int j = 0; j < i; j++) {
                free(pg_handle->server_list[j]);
            }
            free(pg_handle->server_list);
            return -1;
        }
    }
    pg_handle->connected = 0;
    pg_handle->left_conn = NULL;
    pg_handle->right_conn = NULL;
    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "No IB devices found\n");
        return -1;
    }
    ib_dev = dev_list[0];
    if (!ib_dev) {
        fprintf(stderr, "No IB device found\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    pg_handle->right_conn = pp_init_ctx(ib_dev, RDMA_BUFFER_SIZE, 10, 100, ib_port, 0, 0);
    if (!pg_handle->right_conn) {
        fprintf(stderr, "Failed to initialize right context\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    pg_handle->left_conn = pp_init_ctx(ib_dev, RDMA_BUFFER_SIZE, 10, 100, ib_port, 0, 1);
    if (!pg_handle->left_conn) {
        fprintf(stderr, "Failed to initialize left context\n");
        pp_close_ctx(right_ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    pg_handle->left_conn->routs = pp_post_recv(pg_handle->left_conn, pg_handle->left_conn->rx_depth);
    if(pg_handle->left_conn->routs < pg_handle->left_conn->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", pg_handle->left_conn->routs);
        pp_close_ctx(pg_handle->left_conn);
        pp_close_ctx(pg_handle->right_conn);
        ibv_free_device_list(dev_list);
        return -1;
    }
    pg_handle->right_conn->routs = pp_post_recv(pg_handle->right_conn, pg_handle->right_conn->rx_depth);
    if(pg_handle->right_conn->routs < pg_handle->right_conn->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", pg_handle->right_conn->routs);
        pp_close_ctx(pg_handle->left_conn);
        pp_close_ctx(pg_handle->right_conn);
        ibv_free_device_list(dev_list);
        return -1;
    }
    if (pp_get_port_info(pg_handle->left_conn->context, ib_port, &pg_handle->left_conn->portinfo)) {
        fprintf(stderr, "Couldn't get port info for left connection\n");
        pp_close_ctx(pg_handle->left_conn);
        pp_close_ctx(pg_handle->right_conn);
        ibv_free_device_list(dev_list);
        return -1;
    }
    if (pp_get_port_info(pg_handle->right_conn->context, ib_port, &pg_handle->right_conn->portinfo)) {
        fprintf(stderr, "Couldn't get port info for right connection\n");
        pp_close_ctx(pg_handle->left_conn);
        pp_close_ctx(pg_handle->right_conn);
        ibv_free_device_list(dev_list);
        return -1;
    }
    my_dest_left.lid = pg_handle->left_conn->portinfo.lid;
    my_dest_left.qpn = pg_handle->left_conn->qp->qp_num;
    my_dest_left.psn = rand() & 0xffffff;

    my_dest_right.lid = pg_handle->right_conn->portinfo.lid;
    my_dest_right.qpn = pg_handle->right_conn->qp->qp_num;
    my_dest_right.psn = rand() & 0xffffff;
   

    if(pg_handle->server_idx == 0){
        rem_dest_right = pp_client_exch_dest(pg_handle->server_list[1], port, &my_dest_right);
        if (!rem_dest_right) {
            fprintf(stderr, "Server couldn't exchange left destination\n");
            pp_close_ctx(pg_handle->left_conn);
            pp_close_ctx(pg_handle->right_conn);
            ibv_free_device_list(dev_list);
            return -1;
        }
        rem_dest_left = pp_server_exch_dest(pg_handle->left_conn, ib_port, mtu, port, 0, &my_dest_left, -1);
        if (!rem_dest_left) {
            fprintf(stderr, "Server couldn't exchange right destination\n");
            pp_close_ctx(pg_handle->left_conn);
            pp_close_ctx(pg_handle->right_conn);
            ibv_free_device_list(dev_list);
            return -1;
        }
    
    }
    else{
        rem_dest_left = pp_server_exch_dest(pg_handle->left_conn, ib_port, mtu, port, 0, &my_dest_left, -1);
        if (!rem_dest_left) {
            fprintf(stderr, "Server couldn't exchange left destination\n");
            pp_close_ctx(pg_handle->left_conn);
            pp_close_ctx(pg_handle->right_conn);
            ibv_free_device_list(dev_list);
            return -1;
        }
        int server_idx_right = (pg_handle->server_idx + 1) % pg_handle->num_servers;
        rem_dest_right = pp_client_exch_dest(pg_handle->server_list[server_idx_right], port, &my_dest_right);
        if (!rem_dest_right) {
            fprintf(stderr, "Server couldn't exchange right destination\n");
            pp_close_ctx(pg_handle->left_conn);
            pp_close_ctx(pg_handle->right_conn);
            ibv_free_device_list(dev_list);
            return -1;
        }
    }
    pp_connect_ctx(pg_handle->right_conn, ib_port, my_dest_right.psn, mtu, 0, rem_dest_right, -1);
    
    memset(pg_handle->right_conn->buf, idx + 1, pg_handle->right_conn->size);
    pp_post_send(pg_handle->right_conn);
    printf("Server %d sent data to right neighbor\n", pg_handle->server_idx);
    pp_wait_completions(pg_handle->right_conn, 1);
    printf("Server %d received data from right neighbor\n", pg_handle->server_idx);
    pp_wait_completions(pg_handle->left_conn, 1);
    // print the first 5 elements incoming from the left neighbor
    printf("Left neighbor sent: ");
    for (int i = 0; i < 5 && i < pg_handle->left_conn->size; i++) {
        printf("%02x ", ((unsigned char *)pg_handle->left_conn->buf)[i]);
    }
    printf("\n");

}
