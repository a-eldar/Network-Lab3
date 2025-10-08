#ifndef PG_CLOSE_H
#define PG_CLOSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pg_handle.h"

/**
 * @brief Close and cleanup process group RDMA connections
 * 
 * This function cleans up all RDMA resources associated with the process group:
 * - Destroys RDMA connections (left and right neighbors)
 * - Frees work buffers
 * - Resets connection status
 * 
 * @param pg_handle Pointer to the process group handle to close
 * @return 0 on success, -1 on failure
 */
int pg_close(PGHandle* pg_handle);

#ifdef __cplusplus
}
#endif

#endif /* PG_CLOSE_H */
