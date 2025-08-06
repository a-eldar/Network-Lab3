#include <netdb.h>
#include <stdio.h>

int main() {
    struct addrinfo test;
    printf("addrinfo size: %zu\n", sizeof(test));
    return 0;
}