#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>           // struct addrinfo
#include <arpa/inet.h>       // inet_ntop
#include <time.h>

#include "tcp_exchange.h"

int resolve_hostname_to_ip(const char *hostname, char *ip_str, size_t ip_str_size) {
    struct addrinfo hints, *result;
    struct sockaddr_in *addr_in;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    
    int status = getaddrinfo(hostname, NULL, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }
    
    addr_in = (struct sockaddr_in *)result->ai_addr;
    if (!inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, ip_str_size)) {
        perror("inet_ntop");
        freeaddrinfo(result);
        return -1;
    }
    
    freeaddrinfo(result);
    return 0;
}

struct connection_dest* exchange_with_left(const char *left_server, int tcp_port, 
                                          const struct connection_dest *my_dest) {
    struct addrinfo hints, *result, *rp;
    char port_str[16];
    int sockfd = -1;
    struct connection_dest *rem_dest = NULL;
    char msg[sizeof("0000:000000:000000:00000000000000000000000000000000")];
    char gid[33];
    int retry_count = 0;
    const int max_retries = 10;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    snprintf(port_str, sizeof(port_str), "%d", tcp_port);
    
    if (getaddrinfo(left_server, port_str, &hints, &result) != 0) {
        fprintf(stderr, "getaddrinfo failed for %s:%d\n", left_server, tcp_port);
        return NULL;
    }
    
    // Try to connect with retries
    while (retry_count < max_retries) {
        for (rp = result; rp != NULL; rp = rp->ai_next) {
            sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sockfd == -1) continue;
            
            if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
                break; // Success
            }
            
            close(sockfd);
            sockfd = -1;
        }
        
        if (sockfd != -1) break; // Connected successfully
        
        retry_count++;
        sleep(1); // Wait 1 second before retry
        printf("Retrying connection to %s (attempt %d/%d)\n", left_server, retry_count, max_retries);
    }
    
    freeaddrinfo(result);
    
    if (sockfd == -1) {
        fprintf(stderr, "Could not connect to %s:%d after %d attempts\n", 
                left_server, tcp_port, max_retries);
        return NULL;
    }
    
    // Send my destination info
    gid_to_wire_gid(&my_dest->gid, gid);
    snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 
             my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    
    if (send(sockfd, msg, sizeof(msg) - 1, 0) != sizeof(msg) - 1) {
        perror("send");
        close(sockfd);
        return NULL;
    }
    
    // Receive remote destination info
    if (recv(sockfd, msg, sizeof(msg) - 1, 0) != sizeof(msg) - 1) {
        perror("recv");
        close(sockfd);
        return NULL;
    }
    
    // Send acknowledgment
    send(sockfd, "done", 4, 0);
    
    rem_dest = malloc(sizeof(*rem_dest));
    if (!rem_dest) {
        close(sockfd);
        return NULL;
    }
    
    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);
    
    close(sockfd);
    return rem_dest;
}

struct connection_dest* exchange_with_right(int tcp_port, const struct connection_dest *my_dest) {
    struct addrinfo hints, *result, *rp;
    char port_str[16];
    int sockfd = -1, connfd = -1;
    struct connection_dest *rem_dest = NULL;
    char msg[sizeof("0000:000000:000000:00000000000000000000000000000000")];
    char gid[33];
    int opt = 1;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    snprintf(port_str, sizeof(port_str), "%d", tcp_port);
    
    if (getaddrinfo(NULL, port_str, &hints, &result) != 0) {
        fprintf(stderr, "getaddrinfo failed for port %d\n", tcp_port);
        return NULL;
    }
    
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;
        
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            perror("setsockopt");
            close(sockfd);
            continue;
        }
        
        if (bind(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Success
        }
        
        close(sockfd);
        sockfd = -1;
    }
    
    freeaddrinfo(result);
    
    if (sockfd == -1) {
        fprintf(stderr, "Could not bind to port %d\n", tcp_port);
        return NULL;
    }
    
    if (listen(sockfd, 1) == -1) {
        perror("listen");
        close(sockfd);
        return NULL;
    }
    
    printf("Listening on port %d for right neighbor...\n", tcp_port);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);
    
    if (connfd == -1) {
        perror("accept");
        return NULL;
    }
    
    // Receive remote destination info
    if (recv(connfd, msg, sizeof(msg) - 1, 0) != sizeof(msg) - 1) {
        perror("recv");
        close(connfd);
        return NULL;
    }
    
    rem_dest = malloc(sizeof(*rem_dest));
    if (!rem_dest) {
        close(connfd);
        return NULL;
    }
    
    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);
    
    // Send my destination info
    gid_to_wire_gid(&my_dest->gid, gid);
    snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 
             my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    
    if (send(connfd, msg, sizeof(msg) - 1, 0) != sizeof(msg) - 1) {
        perror("send");
        free(rem_dest);
        close(connfd);
        return NULL;
    }
    
    // Wait for acknowledgment
    recv(connfd, msg, 4, 0);
    
    close(connfd);
    return rem_dest;
}