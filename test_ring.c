#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pg.h"

/* Simple test: constructs host list from argv, form process group,
   each process fills sendbuf with value (rank+1), calls all-reduce,
   verifies that each element equals sum_{ranks}(rank+1).
*/

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <host1> <host2> ... <count(optional)>\n", argv[0]);
        return 1;
    }
    int myrank = -1;
    int i = 1;
    char hostlist_str[4096]; hostlist_str[0]=0;
    int count = 16;
    while (i < argc) {
        if (strcmp(argv[i], "-myindex") == 0) {
            myrank = atoi(argv[i+1]) - 1; /* allow 1-based like exercise doc */
            i += 2;
        } else if (strcmp(argv[i], "-list") == 0) {
            i++;
            while (i < argc && argv[i][0] != '-') {
                if (hostlist_str[0]) strcat(hostlist_str, " ");
                strcat(hostlist_str, argv[i]);
                i++;
            }
        } else if (strcmp(argv[i], "-count") == 0) {
            count = atoi(argv[i+1]);
            i += 2;
        } else {
            i++;
        }
    }
    if (myrank < 0) { fprintf(stderr, "missing -myindex\n"); return 1; }
    if (hostlist_str[0] == 0) { fprintf(stderr, "missing -list\n"); return 1; }

    printf("myrank=%d hostlist='%s' count=%d\n", myrank, hostlist_str, count);

    void *pg = NULL;
    if (connect_process_group(hostlist_str, myrank, &pg) != 0) {
        fprintf(stderr, "connect_process_group failed\n"); return 1;
    }
    int32_t *send = malloc(count * sizeof(int32_t));
    int32_t *recv = malloc(count * sizeof(int32_t));
    for (int j = 0; j < count; ++j) send[j] = myrank + 1; /* easy values */
    memset(recv, 0, count * sizeof(int32_t));

    if (pg_all_reduce(send, recv, count, sizeof(int32_t), myrank, pg) != 0) {
        fprintf(stderr, "pg_all_reduce failed\n");
        pg_close(pg);
        return 1;
    }

    /* expected sum is sum_{r=0..n-1} (r+1) = n*(n+1)/2 */
    /* To get n, we must parse hostlist count: naive split */
    int nprocs = 0;
    char *copy = strdup(hostlist_str);
    char *tok = strtok(copy, " ");
    while (tok) { nprocs++; tok = strtok(NULL, " "); }
    free(copy);
    int expected = nprocs * (nprocs + 1) / 2;

    int ok = 1;
    for (int j = 0; j < count; ++j) {
        if (recv[j] != expected) {
            ok = 0;
            fprintf(stderr, "recv[%d] = %d expected %d\n", j, recv[j], expected);
            break;
        }
    }
    if (ok) {
        printf("Allreduce success: every element == %d\n", expected);
    } else {
        printf("Allreduce failed\n");
    }

    pg_close(pg);
    free(send);
    free(recv);
    return ok ? 0 : 2;
}
