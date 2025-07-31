#ifndef CONNECTION_H
#define CONNECTION_H
#include "api.h"

int setup_ring_connections(char **server_names, int num_servers, int my_index, pg_handle_t *pg);
int setup_peer_connection(pg_handle_t *pg, rdma_peer_t *peer, const char *hostname);

#endif // CONNECTION_H