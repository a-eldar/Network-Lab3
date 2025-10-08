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

/**
 * RDMA-Writes the send buf to the right neighbor in a ring topology using RDMA Write.
 * @note Requires that the sendbuf is already populated with the message to send.
 * @param pg_handle Pointer to the process group handle.
 * @return 0 on success, 1 on failure.
 */
int rdma_write_to_right(pg_handle_t *pg_handle);  

/**
 * Polls the completion queue for a work completion.
 * @param pg_handle Pointer to the process group handle.
 * @return 0 on success, 1 on failure.
 */
int poll_for_completion(pg_handle_t *pg_handle);
#endif // RDMA_UTILS_H