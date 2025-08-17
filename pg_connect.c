
#include "pg_handle.h"
#include <stdio.h>
#include <stdarg.h>

#define DEBUG 1

void print_debug(const char *format, ...){
    if (DEBUG) {
        va_list args;
        va_start(args, format);
        fprintf(stderr, "DEBUG: ");
         // Print the formatted string to stderr
        vfprintf(stderr, format, args);
        va_end(args);
    }
}

int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle){
    if (len <= 0 || idx < 0 || idx >= len) {
        fprintf(stderr, "Invalid server list or index\n");
        return -1;
    }

    pg_handle->num_servers = len;
    pg_handle->server_idx = idx;
    pg_handle->server_list = malloc(len * sizeof(char*));
    if (!pg_handle->server_list) {
        perror("Failed to allocate server list");
        return -1;
    }

    for (int i = 0; i < len; i++) {
        pg_handle->server_list[i] = strdup(serverlist[i]);
        if (!pg_handle->server_list[i]) {
            perror("Failed to copy server name");
            return -1;
        }
    }

    // Initialize RDMA connections
    pg_handle->left_conn = malloc(sizeof(struct pingpong_context));
    pg_handle->right_conn = malloc(sizeof(struct pingpong_context));
    if (!pg_handle->left_conn || !pg_handle->right_conn) {        
        perror("Failed to allocate RDMA contexts");
        return -1;
    }
    pg_handle->connected = 0;

    // Additional connection setup code would go here
    int right_idx = (idx + 1) % len;
    int left_idx = (idx - 1 + len) % len;
    if(idx == 0){
        if(bw(pg_handle->server_list[right_idx], pg_handle->right_conn)){
            fprintf(stderr, "Failed to connect to right neighbor %s\n", pg_handle->server_list[right_idx]);
            return -1;
        } // for the right connection server 0 is the client

        if(bw(NULL, pg_handle->left_conn)){
            fprintf(stderr, "Failed to connect to left neighbor %s\n", pg_handle->server_list[left_idx]);
            return -1;
        }  // for the left connection server 0 is the server
    }
    else{
        if(bw(NULL, pg_handle->left_conn)){
            fprintf(stderr, "Failed to connect to left neighbor %s\n", pg_handle->server_list[left_idx]);
            return -1;
        } // for the left connection this server is the server

        if(bw(pg_handle->server_list[left_idx], pg_handle->right_conn)){
            fprintf(stderr, "Failed to connect to right neighbor %s\n", pg_handle->server_list[right_idx]);
            return -1;
        } // for the right connection this server is the client
    }
    pg_handle->connected = 1;


    
    return 0;
}

int main(int argc, char const *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <num_servers> <server_idx> <server1> [server2] ...\n", argv[0]);
        return 1;
    }
    int num_servers = atoi(argv[1]);
    int server_idx = atoi(argv[2]);
    if (num_servers <= 0 || server_idx < 0 || server_idx >= num_servers) {
        fprintf(stderr, "Invalid number of servers or server index\n");
        return 1;
    }
    char **server_list = malloc(num_servers * sizeof(char*));
    if (!server_list) {
        perror("Failed to allocate server list");
        return 1;
    }
    for (int i = 0; i < num_servers; i++) {
        server_list[i] = strdup(argv[i + 3]);
        if (!server_list[i]) {
            perror("Failed to copy server name");
            for (int j = 0; j < i; j++) {
                free(server_list[j]);
            }
            free(server_list);
            return 1;
        }
    }
    PGHandle pg_handle;
    if (connect_process_group(server_list, num_servers, server_idx, &pg_handle) < 0) {
        fprintf(stderr, "Failed to connect process group\n");
        for (int i = 0; i < num_servers; i++) {
            free(server_list[i]);
        }
        free(server_list);
        return 1;
    }
    printf("Process group connected successfully!\n");
    // Clean up
    for (int i = 0; i < num_servers; i++) {
        free(server_list[i]);
    }
    free(server_list);

    // check if the connections are established by sending a simple message between neighbors
    // copy into the buffer
    memcpy(pg_handle.right_conn->buf, "Hello from left", 15);
    // send the message
    if (pp_post_send(pg_handle.right_conn)) {
        fprintf(stderr, "Failed to post send on right connection\n");
        return 1;
    }
    print_debug("Message sent on right connection: %s\n", (char *)pg_handle.right_conn->buf);
    // wait for completion
    if (pp_wait_completions(pg_handle.right_conn, 1)) {
        fprintf(stderr, "Failed to wait for completion on right connection\n");
        return 1;
    }
    print_debug("Message sent on right connection completed\n");
    // sleep for a while to ensure the message is received
    sleep(1);
    // wait for completion on the left connection
    if (pp_wait_completions(pg_handle.left_conn, 1)) {
        fprintf(stderr, "Failed to wait for completion on left connection\n");
        return 1;
    }
    print_debug("Message received on left connection completed\n");
    // print the message received on the left connection
    printf("Message received on left connection: %s\n", (char *)pg_handle.left_conn->buf);


    return 0;
}


