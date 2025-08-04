#include "rdma_allreduce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: %s <server1> <server2> ... <serverN> <my_index>\n", argv[0]);
        return 1;
    }

    int num_servers = argc - 2;
    int my_index = atoi(argv[argc - 1]);
    char **serverlist = &argv[1];

    PGHandle pg_handle;
    int count = 8; // Number of elements to reduce
    DATATYPE datatype = INT;
    OPERATION op = SUM;

    int *sendbuf = malloc(count * sizeof(int));
    int *recvbuf = malloc(count * sizeof(int));
    if (!sendbuf || !recvbuf) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return 1;
    }

    // Initialize test data
    initialize_test_data(sendbuf, count, datatype, my_index);

    // Connect to process group
    if (connect_process_group(serverlist, num_servers, my_index, &pg_handle) != 0) {
        fprintf(stderr, "Failed to connect process group\n");
        free(sendbuf);
        free(recvbuf);
        return 1;
    }

    // Perform all-reduce
    if (pg_all_reduce(sendbuf, recvbuf, count, datatype, op, &pg_handle) != 0) {
        fprintf(stderr, "All-reduce failed\n");
        pg_close(&pg_handle);
        free(sendbuf);
        free(recvbuf);
        return 1;
    }

    // Print and verify result
    print_buffer_debug(recvbuf, count, datatype, "All-Reduce Result");
    if (verify_allreduce_result(recvbuf, count, datatype, op, num_servers) == 0) {
        printf("All-Reduce result verified successfully!\n");
    } else {
        printf("All-Reduce result verification failed!\n");
    }

    // Cleanup
    pg_close(&pg_handle);
    free(sendbuf);
    free(recvbuf);

    return 0;
}