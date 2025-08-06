#include "pg_handle.h"
#include "rdma_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void print_array(int* arr, int count, const char* label) {
    printf("%s: ", label);
    for (int i = 0; i < count; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
}

void print_double_array(double* arr, int count, const char* label) {
    printf("%s: ", label);
    for (int i = 0; i < count; i++) {
        printf("%.2f ", arr[i]);
    }
    printf("\n");
}

// Two processes. One sends, one receives.
int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <rank> <server1> [server2]\n", argv[0]);
        printf("Example: %s 0 node1 node2\n", argv[0]);
        return 1;
    }
    
    int rank = atoi(argv[1]);
    int num_servers = argc - 2;
    char** serverlist = &argv[2];
    
    printf("Starting process group connection for rank %d with %d servers\n", 
           rank, num_servers);
    
    // Initialize process group
    PGHandle pg_handle;
    if (connect_process_group(serverlist, num_servers, rank, &pg_handle) != 0) {
        fprintf(stderr, "Failed to connect process group\n");
        return 1;
    }
    
    printf("Process group connected successfully!\n");
    
    // If rank is 0, send data; otherwise, receive
    const int count = 10;
    int* sendbuf_int = malloc(count * sizeof(int));
    int* recvbuf_int = malloc(count * sizeof(int));
    if (!sendbuf_int || !recvbuf_int) {
        fprintf(stderr, "Failed to allocate buffers\n");
        pg_close(&pg_handle);
        return 1;
    }
    
    if (rank == 0) {
        // Init send buffer
        for (int i = 0; i < count; i++) {
            sendbuf_int[i] = i + 1; // Rank 0 sends 1, 2, ..., count
        }
        print_array(sendbuf_int, count, "Send buffer (INT)");
        // send data
        printf("Rank 0 sending buffer to rank 1.\n");
        post_send(&pg_handle.right_neighbor, sendbuf_int, count * sizeof(int));
    }
    else {
        // Rank 1 receives data
        printf("Rank 1 waiting for data from rank 0.\n");
        if (wait_for_completion(&pg_handle.left_neighbor, 1) != 0) {
            fprintf(stderr, "Failed to wait for completion\n");
        }
        memcpy(recvbuf_int, pg_handle.left_neighbor.buf, count * sizeof(int));
        print_array(recvbuf_int, count, "Receive buffer (INT)");
    }
     
    // Cleanup
    free(sendbuf_int);
    free(recvbuf_int);
    
    if (pg_close(&pg_handle) != 0) {
        fprintf(stderr, "Failed to close process group properly\n");
        return 1;
    }
    
    printf("Process group closed successfully\n");
    return 0;
}