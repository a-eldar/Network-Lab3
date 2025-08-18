#include "pg_allreduce.h"
#include "pg_connect.h"
#include "pg_collectives.h"

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle) {
	return pg_connect_ring(serverlist, len, idx, pg_handle);
}

