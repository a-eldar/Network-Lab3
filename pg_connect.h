#ifndef PG_CONNECT_H
#define PG_CONNECT_H

#include "rdma_utils.h"
#include "pg_handle.h"

/**
 * @brief Connects a process to its group members in a ring topology
 *
 * @param serverlist Array of server hostnames/IPs in the process group
 * @param len Number of servers in the group
 * @param idx Index of this server in the group (0 to len-1)
 * @param pg_handle Pointer to process group handle structure to be initialized
 * @return int 0 on success, -1 on failure
 *
 * This function:
 * - Initializes RDMA devices and contexts
 * - Sets up connections with left and right neighbors in the ring
 * - Establishes bidirectional communication channels
 * - Allocates and initializes necessary buffers
 */
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);

#endif // PG_CONNECT_H