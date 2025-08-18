// TCP bootstrap and RDMA qp helpers
#ifndef PG_CONNECT_H
#define PG_CONNECT_H

#include "pg_handle.h"

// Establish ring connections using TCP for out-of-band exchange.
// serverlist: array of hostnames/IPs, len: world size, idx: my rank
int pg_connect_ring(char** serverlist, int len, int idx, PGHandle* handle);

#endif // PG_CONNECT_H

