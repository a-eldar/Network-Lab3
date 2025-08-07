#include "pg_allreduce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_index> <total_servers> <server1> [server2] ...\n", argv[0]);
        return 1;
    }
    
    int idx = atoi(argv[1]);
    int total_servers = atoi(argv[2]);
    
    if (argc != total_servers + 3) {
        fprintf(stderr, "Number of server names doesn't match total_servers\n");
        return 1;
    }
    
    // Build server list
    char **serverlist = malloc(total_servers * sizeof(char*));
    for (int i = 0; i < total_servers; i++) {
        serverlist[i] = argv[i + 3];
    }
    
    PGHandle pg_handle;
    
    // Connect to process group
    printf("Server %d: Connecting to process group...\n", idx);
    if (connect_process_group(serverlist, total_servers, idx, &pg_handle) < 0) {
        fprintf(stderr, "Failed to connect to process group\n");
        free(serverlist);
        return 1;
    }
    
    // Test with integers
    int count = 100;
    int *sendbuf = malloc(count * sizeof(int));
    int *recvbuf = malloc(count * sizeof(int));
    
    // Initialize send buffer with server-specific values
    for (int i = 0; i < count; i++) {
        sendbuf[i] = idx + 1; // Each server contributes its index + 1
    }
    
    printf("Server %d: Performing integer sum all-reduce...\n", idx);
    if (pg_all_reduce(sendbuf, recvbuf, count, INT, SUM, &pg_handle) < 0) {
        fprintf(stderr, "All-reduce failed\n");
        pg_close(&pg_handle);
        free(serverlist);
        free(sendbuf);
        free(recvbuf);
        return 1;
    }
    
    // Verify results - sum should be 1+2+...+n = n*(n+1)/2
    int expected = total_servers * (total_servers + 1) / 2;
    int success = 1;
    for (int i = 0; i < count; i++) {
        if (recvbuf[i] != expected) {
            printf("Server %d: Error at index %d: expected %d, got %d\n", 
                   idx, i, expected, recvbuf[i]);
            success = 0;
            break;
        }
    }
    
    if (success) {
        printf("Server %d: Integer sum all-reduce successful! Result = %d\n", idx, expected);
    }
    
    // Test with doubles
    double *dsendbuf = malloc(count * sizeof(double));
    double *drecvbuf = malloc(count * sizeof(double));
    
    for (int i = 0; i < count; i++) {
        dsendbuf[i] = (idx + 1) * 1.5;
    }
    
    printf("Server %d: Performing double sum all-reduce...\n", idx);
    if (pg_all_reduce(dsendbuf, drecvbuf, count, DOUBLE, SUM, &pg_handle) < 0) {
        fprintf(stderr, "Double all-reduce failed\n");
        pg_close(&pg_handle);
        free(serverlist);
        free(sendbuf);
        free(recvbuf);
        free(dsendbuf);
        free(drecvbuf);
        return 1;
    }
    
    double dexpected = total_servers * (total_servers + 1) / 2.0 * 1.5;
    success = 1;
    for (int i = 0; i < count; i++) {
        if (drecvbuf[i] != dexpected) {
            printf("Server %d: Error at index %d: expected %f, got %f\n", 
                   idx, i, dexpected, drecvbuf[i]);
            success = 0;
            break;
        }
    }
    
    if (success) {
        printf("Server %d: Double sum all-reduce successful! Result = %f\n", idx, dexpected);
    }
    
    // Cleanup
    pg_close(&pg_handle);
    
    free(serverlist);
    free(sendbuf);
    free(recvbuf);
    free(dsendbuf);
    free(drecvbuf);
    
    printf("Server %d: Test completed\n", idx);
    
    return 0;
}