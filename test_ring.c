#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pg.h"

/* Usage:
   ./test_ring -myindex <0-based-rank> -list host1 host2 ... [-count <elements>]
*/

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s -myindex <rank> -list <host1> <host2> ... [-count <elements>]\n", argv[0]);
        return 1;
    }
    int myrank = -1;
    char hostlist_str[4096] = {0};
    int count = 16;
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-myindex") == 0) { myrank = atoi(argv[i+1]); i += 2; }
        else if (strcmp(argv[i], "-list") == 0) { i++; while (i < argc && argv[i][0] != '-') { if (hostlist_str[0]) strcat(hostlist_str, " "); strcat(hostlist_str, argv[i]); i++; } }
        else if (strcmp(argv[i], "-count") == 0) { count = atoi(argv[i+1]); i += 2; }
        else i++;
    }
    if (myrank < 0) { fprintf(stderr, "missing -myindex\n"); return 1; }
    if (!hostlist_str[0]) { fprintf(stderr, "missing -list\n"); return 1; }

    printf("myrank=%d hosts='%s' count=%d\n", myrank, hostlist_str, count);

    void *pg = NULL;
    if (connect_process_group(hostlist_str, myrank, &pg) != 0) {
        fprintf(stderr, "connect_process_group failed\n"); return 1;
    }

    int32_t *send = malloc(count * sizeof(int32_t));
    int32_t *recv = malloc(count * sizeof(int32_t));
    for (int j = 0; j < count; ++j) send[j] = myrank + 1;
    memset(recv, 0, count * sizeof(int32_t));

    if (pg_all_reduce(send, recv, count, sizeof(int32_t), myrank, pg) != 0) {
        fprintf(stderr, "pg_all_reduce failed\n"); pg_close(pg); return 1;
    }

    /* compute expected: sum_{r=0..n-1} (r+1) */
    int n = 0;
    char *copy = strdup(hostlist_str);
    char *tok = strtok(copy, " ");
    while (tok) { n++; tok = strtok(NULL, " "); }
    free(copy);
    int expected = n * (n + 1) / 2;

    int ok = 1;
    for (int j = 0; j < count; ++j) {
        if (recv[j] != expected) {
            fprintf(stderr, "recv[%d]=%d expected=%d\n", j, recv[j], expected);
            ok = 0; break;
        }
    }
    if (ok) printf("Allreduce success: every element == %d\n", expected);
    else printf("Allreduce FAILED\n");

    pg_close(pg);
    free(send); free(recv);
    return ok ? 0 : 2;
}
