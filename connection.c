#include "connection.h"

int setup_ring_connections(char **server_names, int num_servers, int my_index, pg_handle_t *pg) {
    // 1. Initialize pg_handle
    pg->num_servers = num_servers;
    pg->my_index = my_index;
    pg->server_names = server_names;

    // 2. Determine left and right neighbor indices
    int left_index = (my_index - 1 + num_servers) % num_servers;
    int right_index = (my_index + 1) % num_servers;

    // 3. Get RDMA device and create context, PD, CQ
    pg->device_list = ibv_get_device_list(NULL);
    if (!pg->device_list) return -1;
    pg->ctx = ibv_open_device(pg->device_list[0]); // pick first device
    if (!pg->ctx) return -1;

    pg->pd = ibv_alloc_pd(pg->ctx);
    if (!pg->pd) return -1;

    pg->cq = ibv_create_cq(pg->ctx, /* max_cqe */ 16, NULL, NULL, 0);
    if (!pg->cq) return -1;

    // 4. Setup left and right RDMA connections
    setup_peer_connection(pg, &pg->left_peer, server_names[left_index]);
    setup_peer_connection(pg, &pg->right_peer, server_names[right_index]);

    return 0;
}
