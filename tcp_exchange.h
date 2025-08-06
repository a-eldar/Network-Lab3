#ifndef TCP_EXCHANGE_H
#define TCP_EXCHANGE_H

#include "rdma_utils.h"

// TCP exchange functions
struct connection_dest* exchange_with_left(const char *left_server, int tcp_port, 
                                          const struct connection_dest *my_dest);
struct connection_dest* exchange_with_right(int tcp_port, const struct connection_dest *my_dest);
int resolve_hostname_to_ip(const char *hostname, char *ip_str, size_t ip_str_size);

#endif // TCP_EXCHANGE_H