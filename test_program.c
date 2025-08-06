#include "pg_handle.h"
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

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <rank> <server1> [server2] [server3] ...\n", argv[0]);
        printf("Example: %s 0 node1 node2 node3\n", argv[0]);
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
    
    // Test with integer data
    const int count = 10;
    int* sendbuf_int = malloc(count * sizeof(int));
    int* recvbuf_int = malloc(count * sizeof(int));
    
    // Initialize send buffer with rank-specific values
    for (int i = 0; i < count; i++) {
        sendbuf_int[i] = rank * 10 + i;
    }
    
    print_array(sendbuf_int, count, "Send buffer (INT)");
    
    // Perform all-reduce with SUM operation
    printf("Performing INT SUM all-reduce...\n");
    if (pg_all_reduce(sendbuf_int, recvbuf_int, count, INT, SUM, &pg_handle) != 0) {
        fprintf(stderr, "All-reduce failed\n");
        free(sendbuf_int);
        free(recvbuf_int);
        pg_close(&pg_handle);
        return 1;
    }
    
    print_array(recvbuf_int, count, "Receive buffer (INT SUM)");
    
    // Test with double data
    double* sendbuf_double = malloc(count * sizeof(double));
    double* recvbuf_double = malloc(count * sizeof(double));
    
    // Initialize send buffer with rank-specific values
    for (int i = 0; i < count; i++) {
        sendbuf_double[i] = (rank + 1) * 1.5 + i * 0.1;
    }
    
    print_double_array(sendbuf_double, count, "Send buffer (DOUBLE)");
    
    // Perform all-reduce with SUM operation
    printf("Performing DOUBLE SUM all-reduce...\n");
    if (pg_all_reduce(sendbuf_double, recvbuf_double, count, DOUBLE, SUM, &pg_handle) != 0) {
        fprintf(stderr, "All-reduce failed\n");
        free(sendbuf_int);
        free(recvbuf_int);
        free(sendbuf_double);
        free(recvbuf_double);
        pg_close(&pg_handle);
        return 1;
    }
    
    print_double_array(recvbuf_double, count, "Receive buffer (DOUBLE SUM)");
    
    // Test with multiplication
    for (int i = 0; i < count; i++) {
        sendbuf_int[i] = rank + 1; // Each rank contributes (rank+1)
    }
    
    print_array(sendbuf_int, count, "Send buffer for MULT");
    
    printf("Performing INT MULT all-reduce...\n");
    if (pg_all_reduce(sendbuf_int, recvbuf_int, count, INT, MULT, &pg_handle) != 0) {
        fprintf(stderr, "All-reduce multiplication failed\n");
        free(sendbuf_int);
        free(recvbuf_int);
        free(sendbuf_double);
        free(recvbuf_double);
        pg_close(&pg_handle);
        return 1;
    }
    
    print_array(recvbuf_int, count, "Receive buffer (INT MULT)");
    
    printf("All tests completed successfully!\n");
    
    // Cleanup
    free(sendbuf_int);
    free(recvbuf_int);
    free(sendbuf_double);
    free(recvbuf_double);
    
    if (pg_close(&pg_handle) != 0) {
        fprintf(stderr, "Failed to close process group properly\n");
        return 1;
    }
    
    printf("Process group closed successfully\n");
    return 0;
}