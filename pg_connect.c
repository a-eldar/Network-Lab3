
#include "pg_handle.h"


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
    
    return 0;
}

