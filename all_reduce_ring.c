#include "all_reduce_ring.h"

#include <iso646.h>
#include <stdlib.h>  // for EXIT_SUCCESS, EXIT_FAILURE
#include <stdio.h>
#include <math.h>  // for pow
#include <string.h>  // for memset
#include <netinet/in.h>  // for htonl


void debug_print(char* message) {
    if (DEBUG_MODE) {
        fprintf(stdout, "%s\n", message);
    }
}

void test_default_data_after_procedure(void* data, const int count, const int ring_size, const DATATYPE datatype) {
    float reference_value = 0;
    for (int i = 0; i < ring_size; ++i) {
        reference_value += (float)pow(10.0, i);
    }
    if (DEFAULT_OPERATION == MEAN) {
        reference_value /= ring_size;
    }

    int unequal_cells = 0;
    for (int i = 0; i < count; ++i) {
        switch (datatype) {
            case INT:
                unequal_cells += (((int*)data)[i] == (int)reference_value) ? 0 : 1;
            break;
            case FLOAT:
                unequal_cells += (((float*)data)[i] == (float)reference_value) ? 0 : 1;
            break;
            case DOUBLE:
                unequal_cells += (((double*)data)[i] == (double)reference_value) ? 0 : 1;
            break;
        }
    }
    if (unequal_cells) {
        printf("%d out of %d cells don't have the same value as reference\n", unequal_cells, count);
    } else {
        printf("Success! All cells have the value of the reference!\n");
    }
    if (DEBUG_MODE) { // print by default data type
        const char* format = "Wanted reference value: %f\nFirst cell value: %f\nLast cell value: %f\n";

        if (datatype == INT) {
            printf(format, (float)reference_value, (float)((int*)data)[0], (float)((int*)data)[count - 1]);
        } else if (datatype == FLOAT) {
            printf(format, reference_value, ((float*)data)[0], ((float*)data)[count - 1]);
        } else if (datatype == DOUBLE) {
            printf(format, reference_value, ((double*)data)[0], ((double*)data)[count - 1]);
        }
    }
}

void reduce(void* vec_a, void* vec_b, const int chunk_size, const DATATYPE datatype, const OPERATION op) {
    if (datatype == INT) {
        int *a = (int *)vec_a;
        const int *b = (int *)vec_b;
        for (int i = 0; i < chunk_size; i++) {
            switch (op) {
                case SUM:
                case MEAN:
                    a[i] += b[i];
                    break;
                case MAX:
                    a[i] = (a[i] > b[i]) ? a[i] : b[i];
                    break;
                case MIN:
                    a[i] = (a[i] < b[i]) ? a[i] : b[i];
                    break;
            }
        }
        if (DEBUG_MODE) {
            printf("After reduction:\ndata[0] == %d, data[chunk_size - 1] == %d, data[chunk_size] == %d, \n",
                   a[0], a[chunk_size - 1], a[chunk_size]);
        }
    } else if (datatype == FLOAT) {
        float *a = (float *)vec_a;
        float *b = (float *)vec_b;
        for (int i = 0; i < chunk_size; i++) {
            switch (op) {
                case SUM:
                case MEAN:
                    a[i] += b[i];
                    break;
                case MAX:
                    a[i] = (a[i] > b[i]) ? a[i] : b[i];
                    break;
                case MIN:
                    a[i] = (a[i] < b[i]) ? a[i] : b[i];
                    break;
            }
        }
        if (DEBUG_MODE) {
            printf("After reduction:\ndata[0] == %g, data[chunk_size - 1] == %g, data[chunk_size] == %g, \n",
                   a[0], a[chunk_size - 1], a[chunk_size]);
        }
    } else if (datatype == DOUBLE) {
        double *a = (double *)vec_a;
        double *b = (double *)vec_b;
        for (int i = 0; i < chunk_size; i++) {
            switch (op) {
                case SUM:
                case MEAN:
                    a[i] += b[i];
                    break;
                case MAX:
                    a[i] = (a[i] > b[i]) ? a[i] : b[i];
                    break;
                case MIN:
                    a[i] = (a[i] < b[i]) ? a[i] : b[i];
                    break;
            }
        }
        if (DEBUG_MODE) {
            printf("After reduction:\ndata[0] == %g, data[chunk_size - 1] == %g, data[chunk_size] == %g, \n",
                   a[0], a[chunk_size - 1], a[chunk_size]);
        }
    }
}


int pg_reduce_scatter(void *sendbuf, void *recvbuf, const int count, const DATATYPE datatype, const OPERATION op, void *pg_handle) {
    struct pg_handle_t *pg = (struct pg_handle_t *)pg_handle;
    const int num_chunks = pg->ring_size;
    const int chunk_size = count / num_chunks;  // Assume count is divisible by ring_size
    const size_t datatype_size = get_datatype_size(datatype);

    struct ibv_wc wc;
    uint32_t chunk_index = -1; // index of the chunk as received from the immediate data

    // Post recieve for the immediate data (no need for a scatter/gather element)
    for (int round = 0; round < num_chunks - 1; round++) {
        struct ibv_recv_wr recv_wr = {
            .sg_list = NULL,
            .num_sge = 0,
        };
        struct ibv_recv_wr *bad_recv_wr;
        if (ibv_post_recv(pg->back.qp, &recv_wr, &bad_recv_wr)) {
            return EXIT_FAILURE;
        }
    }

    for (int round = 0; round < num_chunks - 1; round++) {
        // Calculate chunk indices for this round
        const int send_chunk = (pg->ring_location - round + num_chunks) % num_chunks;

        // Send the chunk
        struct ibv_sge sge_send = {
                .addr = (uint64_t)((char*)sendbuf + send_chunk * chunk_size * datatype_size),
                .length = chunk_size * datatype_size,
                .lkey = pg->send_mr->lkey
        };
        struct ibv_send_wr send_wr = {
            .next       = NULL,                  // No additional WRs
            .sg_list    = &sge_send,                  // Scatter-gather list
            .num_sge    = 1,                     // Single SGE
            .opcode     = IBV_WR_RDMA_WRITE_WITH_IMM,  // Write with Immediate opcode
            // .send_flags = IBV_SEND_SIGNALED,     // Request a completion notification
            .imm_data   = htonl(send_chunk),          // Immediate data (in network byte order)
            .wr.rdma.remote_addr = pg->front_credentials.recvbuf_addr + send_chunk * chunk_size * datatype_size,  // Remote buffer address
            .wr.rdma.rkey        = pg->front_credentials.recvbuf_rkey   // Remote memory key
        };
        struct ibv_send_wr *bad_send_wr;
        if (ibv_post_send(pg->front.qp, &send_wr, &bad_send_wr)) {
            return EXIT_FAILURE;
        }

        // Wait for completion of receiving the chunk
        while (true) {
            if (ibv_poll_cq(pg->back.cq, 1, &wc) > 0) {
                if (wc.status != IBV_WC_SUCCESS) {
                    return EXIT_FAILURE;
                }
                //extract the chunk index from the immediate data
                if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
                    chunk_index = ntohl(wc.imm_data);
                    printf("Received chunk %d\n", chunk_index);
                }
                break;
            }
        }

        // Perform local reduction
        reduce((char*)sendbuf + chunk_index * chunk_size * datatype_size,
               (char*)recvbuf + chunk_index * chunk_size * datatype_size,
               chunk_size, datatype, op);

        // while (true) {
        //     if (ibv_poll_cq(pg->front.cq, 1, &wc) > 0) {
        //         if (wc.status != IBV_WC_SUCCESS) {
        //             return EXIT_FAILURE;
        //         }
        //         break;
        //     }
        // }
        // fprintf(stdout, "Sent chunk %d\n", send_chunk);
    }

    debug_print("Finished all rounds of Reduce-Scatter");

    // For MEAN operation, divide the final chunk by the ring size
    if (op == MEAN) {
        if (datatype == FLOAT) {
            float *data = (float *)(sendbuf + chunk_index * chunk_size * datatype_size);
            for (int j = 0; j < chunk_size; j++) {
                data[j] /= pg->ring_size;
            }
        } else if (datatype == INT) {
            int *data = (int *)(sendbuf + chunk_index * chunk_size * datatype_size);
            for (int j = 0; j < chunk_size; j++) {
                data[j] /= pg->ring_size;
            }
        } else if (datatype == DOUBLE) {
            double *data = (double *)(sendbuf + chunk_index * chunk_size * datatype_size);
            for (int j = 0; j < chunk_size; j++) {
                data[j] /= pg->ring_size;
            }
        }
    }

    return EXIT_SUCCESS;
}

int pg_all_gather(void *sendbuf, const int count, const DATATYPE datatype, void *pg_handle) {
    struct pg_handle_t *pg = (struct pg_handle_t *)pg_handle;
    const int num_chunks = pg->ring_size;
    const int chunk_size = count / num_chunks;  // Assume count is divisible by ring_size
    const size_t datatype_size = get_datatype_size(datatype);

    // Post recieve for the immediate data (no need for a scatter/gather element)
    for (int round = 0; round < num_chunks - 1; round++) {
        struct ibv_recv_wr recv_wr = {
            .sg_list = NULL,
            .num_sge = 0,
        };
        struct ibv_recv_wr *bad_recv_wr;
        if (ibv_post_recv(pg->back.qp, &recv_wr, &bad_recv_wr)) {
            return EXIT_FAILURE;
        }
    }

    for (int round = 0; round < num_chunks - 1; round++) {
        // Calculate chunk indices
        const int send_chunk = (pg->ring_location - round + 1 + num_chunks) % num_chunks;

        // Send the chunk with RDMA write and immediate
        struct ibv_sge sge_send = {
            .addr = (uint64_t)((char*)sendbuf + send_chunk * chunk_size * datatype_size),
            .length = chunk_size * datatype_size,
            .lkey = pg->send_mr->lkey
        };
        struct ibv_send_wr send_wr = {
            .sg_list = &sge_send,
            .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
            .send_flags = IBV_SEND_SIGNALED,
            .imm_data = htonl(send_chunk),
            .wr.rdma.remote_addr = pg->front_credentials.sendbuf_addr + send_chunk * chunk_size * datatype_size,
            .wr.rdma.rkey = pg->front_credentials.sendbuf_rkey
        };
        struct ibv_send_wr *bad_send_wr;
        if (ibv_post_send(pg->front.qp, &send_wr, &bad_send_wr)) {
            return EXIT_FAILURE;
        }

        // Wait for completion
        struct ibv_wc wc;
        int completed = 0;
        while (completed < 2) {
            if (ibv_poll_cq(pg->back.cq, 1, &wc) > 0) {
                if (wc.status != IBV_WC_SUCCESS) {
                    return EXIT_FAILURE;
                }
                completed++;
            }
            if (ibv_poll_cq(pg->front.cq, 1, &wc) > 0) {
                if (wc.status != IBV_WC_SUCCESS) {
                    return EXIT_FAILURE;
                }
                completed++;
            }
        }
    }

    return EXIT_SUCCESS;
}


int pg_all_reduce(void *sendbuf, void *recvbuf, const int count, const DATATYPE datatype, const OPERATION op, void *pg_handle) {
    if (pg_reduce_scatter(sendbuf, recvbuf, count, datatype, op, pg_handle) != 0) {
        perror("Failed to perform the Reduce-Scatter phase");
        return EXIT_FAILURE;
    }
    debug_print("Performed the Reduce-Scatter phase successfully\n");

    if (DEBUG_MODE) {
        const int ring_size = ((struct pg_handle_t*)pg_handle)->ring_size;
        test_default_data_after_procedure(sendbuf, count, ring_size, datatype);
    }

    if (pg_all_gather(sendbuf, count, datatype, pg_handle) != 0) {
        perror("Failed to perform the All-Gather phase");
        return EXIT_FAILURE;
    }
    debug_print("Performed the All-Gather phase successfully");
    return EXIT_SUCCESS;
}

int get_default_data(void** data, DATATYPE* datatype, int* count, OPERATION* op, const int ring_location) {
    // Set the default values
    *count = DEFAULT_COUNT;
    *datatype = DEFAULT_DATATYPE;
    *op = DEFAULT_OPERATION;

    // Calculate the initial value as 10 to the power of ring_location
    const float initial_value = (float)pow(10.0, ring_location);

    // Allocate and initialize memory for "data" based on "datatype" and "count"
    if (*datatype == FLOAT) {
        *data = malloc(*count * sizeof(float));
        if (*data == NULL) {
            perror("Failed to allocate data array");
            return EXIT_FAILURE;
        }
        // Fill the array with the initial value
        for (int i = 0; i < *count; i++) {
            ((float *)*data)[i] = initial_value;
        }
        fprintf(stdout, "Initial value: %f\n", initial_value);
    } else if (*datatype == INT) {
        *data = malloc(*count * sizeof(int));
        if (*data == NULL) {
            perror("Failed to allocate data array");
            return EXIT_FAILURE;
        }
        // Fill the array with the initial value
        const int int_value = (int)initial_value;  // Convert to int if needed
        for (int i = 0; i < *count; i++) {
            ((int *)*data)[i] = int_value;
        }
        fprintf(stdout, "Initial value: %d\n", int_value);
    } else if (*datatype == DOUBLE) {
        *data = malloc(*count * sizeof(double));
        if (*data == NULL) {
            perror("Failed to allocate data array");
            return EXIT_FAILURE;
        }
        // Fill the array with the initial value
        const double double_value = (double)initial_value;  // Convert to double if needed
        for (int i = 0; i < *count; i++) {
            ((double *)*data)[i] = double_value;
        }
        fprintf(stdout, "Initial value: %g\n", double_value);
    } else {
        perror("Unsupported data type");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


void release_data(void** data) {
    free(*data);
    *data = NULL;
}


int main(int argc, char* argv[]) {
    // Usage validation
    if (argc != 4) {
        printf("Usage: %s <ring_size> <ring_location> <servername>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    // Argument parsing
    const int ring_size = atoi(argv[1]);
    const int ring_location = atoi(argv[2]);
    char* servername = argv[3];
    fprintf(stdout, "Starting All Reduce Ring with ring_size: %d, ring_location: %d, "
           "connecting to servername: %s\n", ring_size, ring_location, servername);

    // Connect the process group (initialize pg_handle)
    void* pg_handle_generic;
    if (connect_process_group(servername, &pg_handle_generic, ring_size, ring_location) != 0) {
        perror("Failed to connect process group");
        return EXIT_FAILURE;
    } else {
        debug_print("Connected process group successfully\n");
    }
    struct pg_handle_t* pg_handle = (struct pg_handle_t *) pg_handle_generic;

    // Setup data & parameters for All-Reduce
    void* data;
    DATATYPE datatype;
    int count;
    OPERATION op;
    if (get_default_data(&data, &datatype, &count, &op, pg_handle->ring_location) != 0) {
        pg_close(pg_handle);
        perror("Failed to get default data");
        return EXIT_FAILURE;
    }
    debug_print("Got default data successfully\n");

    // Memory registration
    void *sendbuf, *recvbuf;
    if (register_memory(data, datatype, count, pg_handle, &sendbuf, &recvbuf) != 0) {
        pg_close(pg_handle);
        release_data(data);
        perror("Failed to register memory");
        return EXIT_FAILURE;
    }
    debug_print("Registered memory successfully");

    // exchange registered memory address and key
    if (exchange_registered_memory(pg_handle) != 0) {
        perror("Failed to exchange registered memory");
        unregister_memory(pg_handle, &sendbuf, &recvbuf);
        release_data(&data);
        pg_close(pg_handle);
        return EXIT_FAILURE;
    }
    debug_print("Exchanged memory details successfully\n");

    int return_value = EXIT_SUCCESS;
    // Use pg_handle in other functions
    if (pg_all_reduce(sendbuf, recvbuf, count, datatype, op, pg_handle) != EXIT_SUCCESS) {
        perror("Failed to perform the All-Reduce");
        return_value = EXIT_FAILURE;
    } else {
        debug_print("Finished All-Reduce procedure successfully");
        test_default_data_after_procedure(data, count, ring_size, datatype);
    }

    /// CLEANUP - happens in all cases, doesn't matter the return_value
    // Unregister memory
    if (unregister_memory(pg_handle, &sendbuf, &recvbuf) != 0) {
        perror("Failed to unregister memory");
        return EXIT_FAILURE;
    }
    // Release data
    release_data(&data);
    // Close the process group
    if (pg_close(pg_handle) != EXIT_SUCCESS) {
        perror("Failed to close the PG Handle");
        return EXIT_FAILURE;
    }

    return return_value;
}