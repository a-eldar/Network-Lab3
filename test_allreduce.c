#include "pg_connect.h"
#include "rdma_utils.h"
#include "pg_allreduce.h"
#include "pg_close.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple send/receive test after connect_process_group()
// Each rank writes a message into its right neighbor's buffer
// and then reads back from its left neighbor's buffer.
// Note: Each rank runs this program with a different rank number.

/**
 * Convert the arguments to a server list (comma separated) and my index
 * @param argv
 * @param serverlist
 * @param myindex
 * @return 0 if successful, -1 if error
 */
int convert_args_to_serverlist(char *argv[], char **serverlist, int *myindex) {
    // Initialize output parameters
    *serverlist = NULL;
    *myindex = -1;

    // Parse command line arguments
    for (int i = 1; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "-myindex") == 0 && argv[i+1] != NULL) {
            *myindex = atoi(argv[i+1]);
            i++; // Skip the value we just processed
        }
        else if (strcmp(argv[i], "-list") == 0) {
            // Count number of servers in list
            int num_servers = 0;
            int j = i + 1;
            while (argv[j] != NULL && argv[j][0] != '-') {
                num_servers++;
                j++;
            }

            // Allocate and build comma-separated list
            size_t total_len = 0;
            for (j = i + 1; j < i + 1 + num_servers; j++) {
                total_len += strlen(argv[j]) + 1; // +1 for comma or null
            }

            *serverlist = malloc(total_len);
            if (!*serverlist) {
                return -1;
            }

            char *ptr = *serverlist;
            for (j = i + 1; j < i + 1 + num_servers; j++) {
                int len = strlen(argv[j]);
                memcpy(ptr, argv[j], len);
                ptr += len;
                if (j < i + num_servers) {
                    *ptr = ',';
                    ptr++;
                }
            }
            *ptr = '\0';
            
            break;
        }
    }

    // Validate we got both parameters
    if (*serverlist == NULL || *myindex == -1) {
        if (*serverlist) free(*serverlist);
        return -1;
    }

    return 0;
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <server0> <server1> ...\n", argv[0]);
        return 1;
    }

    char *serverlist;
    int rank;
    if (convert_args_to_serverlist(argv, &serverlist, &rank) != 0) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <server0> <server1> ...\n", argv[0]);
        return 1;
    }

    void *pg_handle_void = NULL;

    printf("Rank %d: Connecting to process group...\n", rank);
    if (connect_process_group(serverlist, &pg_handle_void, rank) != 0) {
        fprintf(stderr, "Rank %d: connect_process_group failed\n", rank);
        return 1;
    }
    
    // Cast handle to the correct type
    PGHandle *pg_handle = (PGHandle *)pg_handle_void;
    
    printf("Rank %d: Connected! Size=%d\n", pg_handle->rank, pg_handle->size);

    int size = 21;
    // Prepare a message in our send buffer
    int* sendbuf = malloc(size * sizeof(int));
    for (int i = 0; i < size; i++) {
        sendbuf[i] = rank*100 + i;
    }

    int *recvbuf = malloc(size * sizeof(int));

    if(pg_all_reduce(sendbuf, recvbuf, size, INT, SUM, pg_handle) != 0) {
        fprintf(stderr, "Rank %d: allreduce failed\n", rank);
        pg_close(pg_handle_void);
        return 1;
    }


    for (int i = 0; i < size; i++) {
        printf("Rank %d: recvbuf[%d] = %d\n", rank, i, recvbuf[i]);
    }

    // Cleanup
    pg_close(pg_handle_void);
    return 0;
}
