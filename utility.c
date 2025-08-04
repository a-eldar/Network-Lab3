// utility.c
#include "rdma_allreduce.h"

size_t get_datatype_size(DATATYPE datatype) {
    switch (datatype) {
        case INT:
            return sizeof(int);
        case DOUBLE:
            return sizeof(double);
        default:
            return 0;
    }
}

void apply_operation(void *dest, void *src, int count, DATATYPE datatype, OPERATION op) {
    if (!dest || !src || count <= 0) {
        return;
    }
    
    switch (datatype) {
        case INT: {
            int *d = (int*)dest;
            int *s = (int*)src;
            for (int i = 0; i < count; i++) {
                switch (op) {
                    case SUM:
                        d[i] += s[i];
                        break;
                    case MULT:
                        d[i] *= s[i];
                        break;
                    default:
                        fprintf(stderr, "Unknown operation for INT type\n");
                        return;
                }
            }
            break;
        }
        case DOUBLE: {
            double *d = (double*)dest;
            double *s = (double*)src;
            for (int i = 0; i < count; i++) {
                switch (op) {
                    case SUM:
                        d[i] += s[i];
                        break;
                    case MULT:
                        d[i] *= s[i];
                        break;
                    default:
                        fprintf(stderr, "Unknown operation for DOUBLE type\n");
                        return;
                }
            }
            break;
        }
        default:
            fprintf(stderr, "Unknown data type\n");
            return;
    }
}

char* resolve_hostname(const char *hostname) {
    if (!hostname) {
        return NULL;
    }
    
    struct hostent *he = gethostbyname(hostname);
    if (!he) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", hostname);
        return NULL;
    }
    
    struct in_addr addr;
    memcpy(&addr, he->h_addr_list[0], sizeof(struct in_addr));
    return inet_ntoa(addr);
}

void print_buffer_debug(void *buffer, int count, DATATYPE datatype, const char *label) {
    if (!buffer || count <= 0 || !label) {
        return;
    }
    
    printf("%s: ", label);
    
    switch (datatype) {
        case INT: {
            int *data = (int*)buffer;
            for (int i = 0; i < count; i++) {
                printf("%d ", data[i]);
            }
            break;
        }
        case DOUBLE: {
            double *data = (double*)buffer;
            for (int i = 0; i < count; i++) {
                printf("%.2f ", data[i]);
            }
            break;
        }
        default:
            printf("Unknown data type");
            break;
    }
    
    printf("\n");
}

int validate_input_parameters(void *sendbuf, void *recvbuf, int count, DATATYPE datatype, OPERATION op) {
    if (!sendbuf || !recvbuf) {
        fprintf(stderr, "Error: sendbuf and recvbuf cannot be NULL\n");
        return -1;
    }
    
    if (count <= 0) {
        fprintf(stderr, "Error: count must be positive\n");
        return -1;
    }
    
    if (datatype != INT && datatype != DOUBLE) {
        fprintf(stderr, "Error: unsupported data type\n");
        return -1;
    }
    
    if (op != SUM && op != MULT) {
        fprintf(stderr, "Error: unsupported operation\n");
        return -1;
    }
    
    return 0;
}

int calculate_ring_neighbors(int rank, int size, int *left_neighbor, int *right_neighbor) {
    if (rank < 0 || rank >= size || size <= 0 || !left_neighbor || !right_neighbor) {
        return -1;
    }
    
    *left_neighbor = (rank - 1 + size) % size;
    *right_neighbor = (rank + 1) % size;
    
    return 0;
}

void initialize_test_data(void *buffer, int count, DATATYPE datatype, int rank) {
    if (!buffer || count <= 0) {
        return;
    }
    
    switch (datatype) {
        case INT: {
            int *data = (int*)buffer;
            for (int i = 0; i < count; i++) {
                data[i] = rank * count + i + 1; // Each rank has unique values
            }
            break;
        }
        case DOUBLE: {
            double *data = (double*)buffer;
            for (int i = 0; i < count; i++) {
                data[i] = (double)(rank * count + i + 1) * 0.5; // Each rank has unique values
            }
            break;
        }
        default:
            fprintf(stderr, "Unknown data type for initialization\n");
            break;
    }
}

int verify_allreduce_result(void *result, int count, DATATYPE datatype, OPERATION op, int num_processes) {
    if (!result || count <= 0 || num_processes <= 0) {
        return -1;
    }
    
    // Create expected result buffer
    void *expected = malloc(count * get_datatype_size(datatype));
    if (!expected) {
        return -1;
    }
    
    // Initialize expected result based on the test data pattern
    switch (datatype) {
        case INT: {
            int *exp = (int*)expected;
            int *res = (int*)result;
            
            for (int i = 0; i < count; i++) {
                if (op == SUM) {
                    exp[i] = 0;
                    for (int rank = 0; rank < num_processes; rank++) {
                        exp[i] += rank * count + i + 1;
                    }
                } else if (op == MULT) {
                    exp[i] = 1;
                    for (int rank = 0; rank < num_processes; rank++) {
                        exp[i] *= rank * count + i + 1;
                    }
                }
                
                if (res[i] != exp[i]) {
                    fprintf(stderr, "Verification failed at index %d: expected %d, got %d\n", 
                            i, exp[i], res[i]);
                    free(expected);
                    return -1;
                }
            }
            break;
        }
        case DOUBLE: {
            double *exp = (double*)expected;
            double *res = (double*)result;
            const double epsilon = 1e-9;
            
            for (int i = 0; i < count; i++) {
                if (op == SUM) {
                    exp[i] = 0.0;
                    for (int rank = 0; rank < num_processes; rank++) {
                        exp[i] += (double)(rank * count + i + 1) * 0.5;
                    }
                } else if (op == MULT) {
                    exp[i] = 1.0;
                    for (int rank = 0; rank < num_processes; rank++) {
                        exp[i] *= (double)(rank * count + i + 1) * 0.5;
                    }
                }
                
                if (fabs(res[i] - exp[i]) > epsilon) {
                    fprintf(stderr, "Verification failed at index %d: expected %.6f, got %.6f\n", 
                            i, exp[i], res[i]);
                    free(expected);
                    return -1;
                }
            }
            break;
        }
        default:
            free(expected);
            return -1;
    }
    
    free(expected);
    return 0;
}