/**
 * ultra_testing.c
 * 
 * Run the all reduce algorithm on vectors,
 * where each vector is a constant value from [2, 3, 5, 7]
 * for ranks [0, 1, 2, 3] respectively.
 * 
 * The expected result is the sum or multiplication of these primes.
 */

#include "pg_connect.h"
#include "rdma_utils.h"
#include "pg_allreduce.h"
#include "pg_close.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#define PRIME_SUM_RESULT_INT 17
#define PRIME_SUM_RESULT_DOUBLE 17.0
#define PRIME_MULT_RESULT_INT 210
#define PRIME_MULT_RESULT_DOUBLE 210.0

// Simple send/receive test after connect_process_group()
// Each rank writes a message into its right neighbor's buffer
// and then reads back from its left neighbor's buffer.
// Note: Each rank runs this program with a different rank number.

/**
 * Convert the arguments to a server list and my index
 * @param argv
 * @param serverlist pointer to list of server names
 * @param myindex pointer to my index
 * @return 0 if successful, -1 if error
 */
int convert_args_to_serverlist(char *argv[], char ***serverlist, int *myindex, int *num_servers) {
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
            *num_servers = 0;
            int j = i + 1;
            while (argv[j] != NULL && argv[j][0] != '-') {
                (*num_servers)++;
                j++;
            }
            
            if (*num_servers == 0) {
                return -1; // No servers provided
            }

            // Allocate memory for server list
            *serverlist = malloc((*num_servers) * sizeof(char*));
            if (!*serverlist) {
                return -1;
            }

            // build list
            for (int k = 0; k < *num_servers; k++) {
                (*serverlist)[k] = argv[i + 1 + k];
            }
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

void fill_vector(void* vector, int size, DATATYPE datatype, int rank) {
    int rank_map_int[] = {2, 3, 5, 7};
    double rank_map_double[] = {2.0, 3.0, 5.0, 7.0};
    if (datatype == INT) {
        int* vec = (int*)vector;
        for (int i = 0; i < size; i++) {
            vec[i] = rank_map_int[rank];
        }
    } else if (datatype == DOUBLE) {
        double* vec = (double*)vector;
        for (int i = 0; i < size; i++) {
            vec[i] = rank_map_double[rank];
        }
    }
}

/**
 * Compares the result vector with what is expected.
 * @param result_vector: pointer to the input vector
 * @param size: size of the input vector
 * @param datatype: datatype of the input vector (INT or DOUBLE)
 * @param op: operation to perform (SUM or MULT)
 * @return true if the result is correct, false otherwise
 */
bool compare_result(void* result_vector, int size, DATATYPE datatype, OPERATION op) {
    if (op == SUM) {
        if (datatype == INT) {
            int* vec = (int*)result_vector;
            for (int i = 0; i < size; i++) {
                if (vec[i] != PRIME_SUM_RESULT_INT) {
                    return false;
                }
            }
        } else if (datatype == DOUBLE) {
            double* vec = (double*)result_vector;
            for (int i = 0; i < size; i++) {
                if (vec[i] != PRIME_SUM_RESULT_DOUBLE) {
                    return false;
                }
            }
        }
    } else if (op == MULT) {
        if (datatype == INT) {
            int* vec = (int*)result_vector;
            for (int i = 0; i < size; i++) {
                if (vec[i] != PRIME_MULT_RESULT_INT) {
                    return false;
                }
            }
        } else if (datatype == DOUBLE) {
            double* vec = (double*)result_vector;
            for (int i = 0; i < size; i++) {
                if (vec[i] != PRIME_MULT_RESULT_DOUBLE) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool test_case(PGHandle* pg_handle, int vector_size, DATATYPE datatype, OPERATION op) {
    // Allocate send and receive buffers
    void* sendbuf = malloc(vector_size * (datatype == INT ? sizeof(int) : sizeof(double)));
    void* recvbuf = malloc(vector_size * (datatype == INT ? sizeof(int) : sizeof(double)));
    if (!sendbuf || !recvbuf) {
        if (sendbuf) free(sendbuf);
        if (recvbuf) free(recvbuf);
        return false;
    }

    // Fill send buffer with rank-specific values
    fill_vector(sendbuf, vector_size, datatype, pg_handle->rank);
    
    // record start time
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    // Perform all-reduce operation
    if (pg_all_reduce(sendbuf, recvbuf, vector_size, datatype, op, pg_handle) != 0) {
        fprintf(stderr, "Rank %d: allreduce failed\n", pg_handle->rank);
        free(sendbuf);
        free(recvbuf);
        return false;
    }
    // record end time
    clock_gettime(CLOCK_MONOTONIC, &end);
    double total_time = (end.tv_sec - start.tv_sec) + 
        (end.tv_nsec - start.tv_nsec) / 1e9;
    // calculate throughput
    double data_size = vector_size * (datatype == INT ? sizeof(int) : sizeof(double));
    double throughput = data_size / total_time;
    printf("Rank %d: allreduce completed in %.6f seconds, throughput: %.2f bytes/second\n", 
           pg_handle->rank, total_time, throughput);

    // Compare result with expected value
    bool result = compare_result(recvbuf, vector_size, datatype, op);

    // Free buffers
    free(sendbuf);
    free(recvbuf);

    return result;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <server0> <server1> ...\n", argv[0]);
        return 1;
    }

    char **serverlist;
    int rank;
    int num_servers;
    if (convert_args_to_serverlist(argv, &serverlist, &rank, &num_servers) != 0) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <server0> <server1> ...\n", argv[0]);
        return 1;
    }

    void *pg_handle_void = NULL;

    printf("Rank %d: Connecting to process group...\n", rank);
    if (connect_process_group(serverlist, num_servers, &pg_handle_void, rank) != 0) {
        fprintf(stderr, "Rank %d: connect_process_group failed\n", rank);
        return 1;
    }
    
    // Cast handle to the correct type
    PGHandle *pg_handle = (PGHandle *)pg_handle_void;
    
    int size = 4;
    for (int i = 0; i < 20; i++) {
        size *= 2;
        printf("Rank %d: Testing vector size %d, INT, SUM...\n", rank, size);
        if (!test_case(pg_handle, size, INT, SUM)) {
            fprintf(stderr, "Rank %d: Test case failed for size %d, INT,SUM\n", rank, size);
    
        }
        printf("Rank %d: Testing vector size %d, INT, MULT...\n", rank, size);
        if (!test_case(pg_handle, size, INT, MULT)) {
            fprintf(stderr, "Rank %d: Test case failed for size %d, INT,MULT\n", rank, size);
        }
        printf("Rank %d: Testing vector size %d, DOUBLE, SUM...\n", rank, size);
        if (!test_case(pg_handle, size, DOUBLE, SUM)) {
            fprintf(stderr, "Rank %d: Test case failed for size %d, DOUBLE,SUM\n", rank, size);
        }
        printf("Rank %d: Testing vector size %d, DOUBLE, MULT...\n", rank, size);
        if (!test_case(pg_handle, size, DOUBLE, MULT)) {
            fprintf(stderr, "Rank %d: Test case failed for size %d, DOUBLE,MULT\n", rank, size);
        }
    }
}
