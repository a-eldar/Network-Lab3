#include "pg_connect.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple send/receive test after connect_process_group()
// Each rank writes a message into its right neighbor's buffer
// and then reads back from its left neighbor's buffer.
// Note: Each rank runs this program with a different rank number.

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <comma_separated_server_list> <rank>\n", argv[0]);
        return 1;
    }

    char *serverlist = argv[1];
    int rank = atoi(argv[2]);

    void *pg_handle_void = NULL;

    printf("Rank %d: Connecting to process group...\n", rank);
    if (connect_process_group(serverlist, &pg_handle_void, rank) != 0) {
        fprintf(stderr, "Rank %d: connect_process_group failed\n", rank);
        return 1;
    }
    
    // Cast handle to the correct type
    pg_handle_t *pg_handle = (pg_handle_t *)pg_handle_void;
    
    printf("Rank %d: Connected! Size=%d\n", pg_handle->rank, pg_handle->size);

    // Get neighbors (ring topology)
    int right_neighbor = (rank + 1) % pg_handle->size;
    int left_neighbor = (rank - 1 + pg_handle->size) % pg_handle->size;
    
    printf("Rank %d: Left neighbor = %d, Right neighbor = %d\n", 
           rank, left_neighbor, right_neighbor);

    // Prepare a message in our send buffer
    char message[256];
    snprintf(message, sizeof(message), "Hello from rank %d!", rank);
    
    if(rdma_write_to_right(pg_handle, message, strlen(message) + 1)){
        fprintf(stderr, "Rank %d: rdma_write_to_right failed\n", rank);
        pg_close(pg_handle_void);
        return 1;
    }
    // Wait for completion
    if(poll_for_completion(pg_handle) != 0) {
        fprintf(stderr, "Rank %d: poll_for_completion failed\n", rank);
        pg_close(pg_handle_void);
        return 1;
    }

    // Small delay to let neighbors write
    sleep(1);

    // Print what we received in our recvbuf (left neighbor should have written here)
    printf("Rank %d: Received buffer = \"%s\"\n", rank, (char *)pg_handle->recvbuf);

    // Cleanup
    pg_close(pg_handle_void);

    return 0;
}
