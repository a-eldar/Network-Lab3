#include "rdma_utils.h"

#include <stdio.h>  // for atoi
#include <stdlib.h>  // for EXIT_SUCCESS, EXIT_FAILURE
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))

// define a message string "0000:000000:000000:00000000000000000000000000000000"
#define TCP_MSG_FORMAT "0000:000000:000000"
#define ACK_MESSAGE "done"

int null_func(void* data) {
    if (data == NULL) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/**
 * get_datatype_size - Returns the size of the datatype in bytes.
 */
size_t get_datatype_size(const DATATYPE datatype) {
    switch (datatype) {
        case INT:
            return sizeof(int);
        case FLOAT:
            return sizeof(float);
        case DOUBLE:
            return sizeof(double);
    }
    return EXIT_FAILURE;
}

/**
 * setup_process_group_handle_and_context - Allocates memory for the process group handle and creates the RDMA context.
 */
static int setup_process_group_handle_and_context(void **pg_handle) {
    // Getting the RDMA device
    struct ibv_device **dev_list = ibv_get_device_list(NULL); // RDMA devices available on the system
    if (!dev_list) {perror("Failed to get IB devices list"); return EXIT_FAILURE;}

    struct ibv_device *ib_dev = *dev_list; // First InfiniBand device in the list
    if (!ib_dev) {fprintf(stderr, "No IB devices found\n"); goto out_fail;}

    // Allocate memory for pg_handle
    *pg_handle = malloc(sizeof(struct pg_handle_t));
    if (*pg_handle == NULL) {perror("Failed to allocate memory for pg_handle"); goto out_fail;}
    struct pg_handle_t *pg = (struct pg_handle_t *) *pg_handle;

    // Create the RDMA context
    pg->context = ibv_open_device(ib_dev);
    if (!pg->context) {
        fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
        free(pg);
        goto out_fail;
    }

    ibv_free_device_list(dev_list);
    return EXIT_SUCCESS;

    out_fail:
            ibv_free_device_list(dev_list);
    return EXIT_FAILURE;
}

/**
 * pg_close - Destroys the QP and deallocates the protection domain.
 */
int pg_close(void *pg_handle) {
    struct pg_handle_t *pg = (struct pg_handle_t *)pg_handle;
    int return_value = EXIT_SUCCESS;

    // First, flush any pending completions from the CQs
    struct ibv_wc wc;
    while (pg->front.cq && ibv_poll_cq(pg->front.cq, 1, &wc) > 0) {
        // Just drain the completions, write debug message if needed
        if (DEBUG_MODE) {
            fprintf(stdout, "Draining front CQ\n");
        }
    }
    while (pg->back.cq && ibv_poll_cq(pg->back.cq, 1, &wc) > 0) {
        // Just drain the completions, write debug message if needed
        if (DEBUG_MODE) {
            fprintf(stdout, "Draining back CQ\n");
        }
    }

    // Destroy QPs first (must be done before destroying CQs)
    if (pg->front.qp) {
        // First transition QP to ERROR state
        struct ibv_qp_attr attr = {
                .qp_state = IBV_QPS_ERR
        };
        if (ibv_modify_qp(pg->front.qp, &attr, IBV_QP_STATE)) {
            fprintf(stderr, "Failed to modify front QP to ERROR state\n");
            return_value = EXIT_FAILURE;
        }
        if (ibv_destroy_qp(pg->front.qp)) {
            fprintf(stderr, "Failed to destroy front QP\n");
            return_value = EXIT_FAILURE;
        }
    }

    if (pg->back.qp) {
        // First transition QP to ERROR state
        struct ibv_qp_attr attr = {
                .qp_state = IBV_QPS_ERR
        };
        if (ibv_modify_qp(pg->back.qp, &attr, IBV_QP_STATE)) {
            fprintf(stderr, "Failed to modify back QP to ERROR state\n");
            return_value = EXIT_FAILURE;
        }
        if (ibv_destroy_qp(pg->back.qp)) {
            fprintf(stderr, "Failed to destroy back QP\n");
            return_value = EXIT_FAILURE;
        }
    }

    // Now destroy CQs
    if (pg->front.cq) {
        if (ibv_destroy_cq(pg->front.cq)) {
            fprintf(stderr, "Failed to destroy front CQ\n");
            return_value = EXIT_FAILURE;
        }
    }

    if (pg->back.cq) {
        if (ibv_destroy_cq(pg->back.cq)) {
            fprintf(stderr, "Failed to destroy back CQ\n");
            return_value = EXIT_FAILURE;
        }
    }

    // Then deallocate PD
    if (pg->pd) {
        if (ibv_dealloc_pd(pg->pd)) {
            fprintf(stderr, "Failed to deallocate PD\n");
            return_value = EXIT_FAILURE;
        }
    }

    // Finally close the device
    if (pg->context) {
        if (ibv_close_device(pg->context)) {
            fprintf(stderr, "Failed to close device\n");
            return_value = EXIT_FAILURE;
        }
    }

    free(pg);
    return return_value;
}


/**
 * create_and_init_qp - Creates a Queue Pair (QP) under a given process group
 * handle and into a given pointer, then changes the QP state to INIT.
 */
static int create_and_init_qp(struct pg_handle_t *pg, struct ibv_qp **qp, const uint8_t max_wqe) {
    // Create completion queue
    struct ibv_cq *cq = ibv_create_cq(pg->context, 2*max_wqe, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return EXIT_FAILURE;
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap     = {
            .max_send_wr  = max_wqe,
            .max_recv_wr  = max_wqe,
            .max_send_sge = 1,
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };

    *qp = ibv_create_qp(pg->pd, &qp_attr);
    if (!*qp) {
        fprintf(stderr, "Couldn't create QP\n");
        return EXIT_FAILURE;
    }

    // Change QPs state to init
    struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,
            .pkey_index      = 0,
            .port_num        = IB_PORT,
            .qp_access_flags = 0
    };

    if (ibv_modify_qp(*qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/**
 * setup_process_group_pd_and_qps - Allocates protection domain and creates QPs for front and back connections.
 */
static int setup_process_group_pd_and_qps(struct pg_handle_t *pg, const uint8_t max_elements) {
    // Allocate protection domain
    pg->pd = ibv_alloc_pd(pg->context);
    if (!pg->pd) {
        perror("Failed to allocate protection domain");
        return EXIT_FAILURE;
    }

    // Create QPs for front and back connections and change QPs state to init
    if (create_and_init_qp(pg, &pg->front.qp, 2*max_elements) != 0) {
        fprintf(stderr, "Failed to create front QP\n");
        return EXIT_FAILURE;
    }
    pg->front.cq = pg->front.qp->send_cq;

    if (create_and_init_qp(pg, &pg->back.qp, max_elements) != 0) {
        fprintf(stderr, "Failed to create back QP\n");
        return EXIT_FAILURE;
    }
    pg->back.cq = pg->back.qp->send_cq;

    // allow remote writing into the back QP
    if (ibv_modify_qp(pg->back.qp, &(struct ibv_qp_attr){
            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE
        }, IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify back QP have write permissions\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/**
 * setup_process_group_local_info - Gets the local LID, QPN, and PSN for the front and back connections.
 */
static int setup_process_group_local_info(struct pg_handle_t *pg) {
    // query port info
    if (ibv_query_port(pg->context, IB_PORT, &pg->port_attr)) {
        fprintf(stderr, "Couldn 't get port info\n");
        return EXIT_FAILURE;
    }

    // get local LID
    pg->front.self_dest.lid = pg->port_attr.lid;
    pg->back.self_dest.lid = pg->port_attr.lid;
    if (pg->port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND && !pg->front.self_dest.lid && !pg->back.self_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return EXIT_FAILURE;
    }

    // get local QPN from QP
    pg->front.self_dest.qpn = pg->front.qp->qp_num;
    pg->back.self_dest.qpn = pg->back.qp->qp_num;

    // Randomly generate a Packet Sequence Number (PSN)
    srand48(getpid() * time(NULL)); // create a seed for the generator based on PID and current time
    pg->front.self_dest.psn = lrand48() & 0xffffff;
    pg->back.self_dest.psn = lrand48() & 0xffffff;

    // Print local details for front and back
    printf(" Front local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
           pg->front.self_dest.lid, pg->front.self_dest.qpn, pg->front.self_dest.psn);
    printf(" Back local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
           pg->back.self_dest.lid, pg->back.self_dest.qpn, pg->back.self_dest.psn);

    return EXIT_SUCCESS;
}

/**
 * set_back_socket_to_listen - Sets the back socket to listen for incoming connections.
 * Based on pp_server_exch_dest.
 */
static void set_back_socket_to_listen(int *sockfd_back) {
    *sockfd_back = -1;                     // Back socket file descriptor
    char *service;                       // Port number as a string
    struct addrinfo *res_back;            // To hold address info for the sockets

    const struct addrinfo hints_back = {              // Address info hints for the back (server) socket configuration
            .ai_flags    = AI_PASSIVE,     // Passive socket for server mode
            .ai_family   = AF_INET,        // Use IPv4
            .ai_socktype = SOCK_STREAM     // TCP socket for reliable stream
    };

    // Convert port numbers to string and store them in `service` variables
    if (asprintf(&service, "%d", TCP_PORT) < 0) {
        perror("Failed to convert port number to string");
        return;
    }

    // Get linked list of address info structures for binding the back socket
    const int n_back = getaddrinfo(NULL, service, &hints_back, &res_back);
    free(service);

    // Check if `getaddrinfo` failed
    if (n_back < 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(n_back));
        fprintf(stderr, "Couldn't get back address info for port %d\n", TCP_PORT);
        return;
    }

    // Iterate through address list to create and bind the back socket
    for (struct addrinfo *t = res_back; t; t = t->ai_next) {
        *sockfd_back = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (*sockfd_back >= 0) {
            int n = 1;
            setsockopt(*sockfd_back, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);
            if (!bind(*sockfd_back, t->ai_addr, t->ai_addrlen))
                break;
            close(*sockfd_back);
            *sockfd_back = -1;
        }
    }
    freeaddrinfo(res_back);

    // Check if socket creation and binding were successful
    if (*sockfd_back < 0) {
        fprintf(stderr, "Couldn't listen backwards to port %d\n", TCP_PORT);
        return;
    }

    // Listen for incoming connections on the back socket
    if (listen(*sockfd_back, 1) < 0) {
        perror("Failed to listen on back socket");
        close(*sockfd_back);
        *sockfd_back = -1;
    }
}

static int pg_connect_qp(struct pg_handle_t *pg, const struct pg_dest *rem_dest, const bool is_front) {
    struct ibv_qp *qp = is_front ? pg->front.qp : pg->back.qp;
    const struct pg_dest *my_dest = is_front ? &pg->front.self_dest : &pg->back.self_dest;

    // Set the qp to RTR (Ready to Receive) state
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_RTR,
        .path_mtu        = IBV_MTU_1024,
        .dest_qp_num     = rem_dest->qpn,
        .rq_psn          = rem_dest->psn,
        .max_dest_rd_atomic  = 1,
        .min_rnr_timer   = 12,
        .ah_attr         = {
            .is_global     = 0,
            .dlid          = rem_dest->lid,
            .sl            = 0,
            .src_path_bits = 0,
            .port_num      = IB_PORT
        }
    };
    if (ibv_modify_qp(qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_AV                 |
            IBV_QP_PATH_MTU           |
            IBV_QP_DEST_QPN           |
            IBV_QP_RQ_PSN             |
            IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP state to RTR\n");
        return EXIT_FAILURE;
    }

    // change state to ready to send
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_dest->psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_TIMEOUT            |
            IBV_QP_RETRY_CNT          |
            IBV_QP_RNR_RETRY          |
            IBV_QP_SQ_PSN             |
            IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP state to RTS\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/**
 * exchange_rdma_information_back - Exchanges RDMA information with the back process.
 *
 * @param pg: The process group handle.
 * @param sockfd_back: The back socket file descriptor.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
static int exchange_rdma_information_back(struct pg_handle_t *pg, const int sockfd_back) {
    char msg[sizeof TCP_MSG_FORMAT];
    struct pg_dest *my_dest = &pg->back.self_dest;

    const int connfd = accept(sockfd_back, NULL, 0);
    close(sockfd_back);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return EXIT_FAILURE;
    }

    // read remote destination
    const ssize_t n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%ld/%ld: Couldn't read back remote address\n", n, sizeof msg);
        goto out_exhange_back;
    }

    struct pg_dest *rem_dest = malloc(sizeof(struct pg_dest));
    if (!rem_dest) {
        fprintf(stderr, "Failed to allocate memory for remote destination\n");
        goto out_exhange_back;
    }

    sscanf(msg, "%x:%x:%x", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn);
    printf(" Back remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
       rem_dest->lid, rem_dest->qpn, rem_dest->psn);

    //connect RDMA QP
    if (pg_connect_qp(pg, rem_dest, false) != 0) {
        fprintf(stderr, "Failed to connect QP to back\n");
        free(rem_dest);
        goto out_exhange_back;
    }
    free(rem_dest);

    // send self destination back
    sprintf(msg, "%04x:%06x:%06x", my_dest->lid, my_dest->qpn, my_dest->psn);
    if (write(connfd, msg, sizeof msg) != sizeof msg) {
        perror("server write");
        fprintf(stderr, "Couldn't send back local address\n");
        goto out_exhange_back;
    }

    // accept ack
    char ack[sizeof ACK_MESSAGE];
    if (read(connfd, ack, sizeof ACK_MESSAGE) != sizeof ACK_MESSAGE) {
        perror("server read");
        fprintf(stderr, "Couldn't read ack message\n");
        goto out_exhange_back;
    }

    close(connfd);
    return EXIT_SUCCESS;

    out_exhange_back:
    close(connfd);
    return EXIT_FAILURE;
}

// Function to set socket to non-blocking mode
static int set_socket_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return EXIT_FAILURE;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// Function to set socket to blocking mode
static int set_socket_blocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return EXIT_FAILURE;
    }
    if (fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL ~O_NONBLOCK");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/**
 * connect_with_poll - Connects to a socket with a timeout using poll.
 */
static int connect_with_poll(int sockfd, struct sockaddr *addr, socklen_t addrlen) {
    if (set_socket_nonblocking(sockfd) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to set socket to non-blocking mode\n");
        return EXIT_FAILURE;
    }

    const int ret = connect(sockfd, addr, addrlen);
    if (ret == 0) {
        if (set_socket_blocking(sockfd) != EXIT_SUCCESS) {
            fprintf(stderr, "Failed to re-set socket to blocking mode\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (errno != EINPROGRESS) {
        perror("connect");
        return EXIT_FAILURE;
    }

    struct pollfd pfd = {.fd = sockfd, .events = POLLOUT};
    fprintf(stdout, "Couldn't connect immediately, waiting for poll\n");

    // wait 10 seconds
    const int poll_result = poll(&pfd, 1, 10000);
    if (poll_result < 0) {
        perror("poll");
        return EXIT_FAILURE;
    } else if (poll_result == 0) {
        fprintf(stderr, "Connection timed out after 10 seconds\n");
        return EXIT_FAILURE;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (pfd.revents & POLLERR) {
            fprintf(stderr, "Poll error: Socket error occurred (POLLERR)\n");
        }
        if (pfd.revents & POLLHUP) {
            fprintf(stderr, "Poll error: Remote side hung up (POLLHUP)\n");
        }
        if (pfd.revents & POLLNVAL) {
            fprintf(stderr, "Poll error: Invalid request on socket (POLLNVAL)\n");
        }
        return EXIT_FAILURE;
    }

    if (set_socket_blocking(sockfd) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to set socket to blocking mode\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Reached end of connect_with_poll\n");

    return EXIT_SUCCESS;
}


/**
 * exchange_rdma_information_front - Exchanges RDMA information with the front process.
 * Based on pp_client_exch_dest.
 *
 * @param pg: The process group handle.
 * @param servername: The name of the server to connect to.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
static int exchange_rdma_information_front(struct pg_handle_t *pg, char *servername) {
    struct addrinfo *res_front;
    struct addrinfo hints_front = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    int sockfd_front = -1;

    // Convert port numbers to string and store them in `service` variables
    if (asprintf(&service, "%d", TCP_PORT) < 0) {
        perror("Failed to convert port number to string the second time");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    const int n_front = getaddrinfo(servername, service, &hints_front, &res_front);
    free(service);

    // Check if `getaddrinfo` failed
    if (n_front < 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(n_front));
        fprintf(stderr, "Couldn't get address info for %s\n", servername);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Start of loop\n");
    for (struct addrinfo *t = res_front; t; t = t->ai_next) {
        sockfd_front = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd_front >= 0) {
            if (!connect_with_poll(sockfd_front, (struct sockaddr *) t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd_front);
            sockfd_front = -1;
        }
    }
    fprintf(stderr, "Reached end of loop\n");

    freeaddrinfo(res_front);

    if (sockfd_front < 0) {
        fprintf(stderr, "Couldn't connect to front server %s:%d\n", servername, TCP_PORT);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Reached 1\n");

    const struct pg_dest my_dest = pg->front.self_dest;
    char msg[sizeof TCP_MSG_FORMAT];
    sprintf(msg, "%04x:%06x:%06x", my_dest.lid, my_dest.qpn, my_dest.psn);
    if (write(sockfd_front, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address while connecting to front server\n");
        goto out_exhange_front;
    }
    fprintf(stderr, "Reached 2\n");

    if (read(sockfd_front, msg, sizeof msg) != sizeof msg) {
        perror("client read");
        fprintf(stdout, "Couldn't read front remote address\n");
        goto out_exhange_front;
    }
    fprintf(stderr, "Reached 3\n");

    write(sockfd_front, ACK_MESSAGE, sizeof ACK_MESSAGE);

    struct pg_dest *rem_dest = malloc(sizeof(struct pg_dest));
    if (!rem_dest) {
        fprintf(stderr, "Failed to allocate memory for remote destination\n");
        goto out_exhange_front;
    }
    fprintf(stderr, "Reached 4\n");

    sscanf(msg, "%x:%x:%x", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn);
    printf(" Front remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n",
       rem_dest->lid, rem_dest->qpn, rem_dest->psn);

    if (pg_connect_qp(pg, rem_dest, true) != 0) {
        fprintf(stderr, "Failed to connect QP to front\n");
        free(rem_dest);
        goto out_exhange_front;
    }
    fprintf(stderr, "Reached end of exchange_rdma_information_front\n");

    free(rem_dest);
    return EXIT_SUCCESS;

    out_exhange_front:
    close(sockfd_front);
    return EXIT_FAILURE;
}

/**
 * connect_process_group - Establishes a process group connection with RDMA support, for each process this means having
 * a front (acting as client) and a back (acting as server) connection.
 * @servername Server name to connect to (front).
 * @pg_handle Pointer to an opaque handle for the process group. Memory for
 *             the handle is allocated within this function.
 *
 * Allocates and initializes the process group handle for RDMA operations,
 * connects to front and back processes using token passing mechanism to avoid deadlock. 
 *
 * Return:
 *   - EXIT_SUCCESS (0) on successful connection.
 *   - EXIT_FAILURE on error (e.g., if pg_handle allocation fails).
 */
int connect_process_group(char *servername, void **pg_handle, const uint8_t ring_size, const uint8_t ring_location) {
    if (setup_process_group_handle_and_context(pg_handle) != 0) {
        fprintf(stderr, "Failed to setup process group handle and context\n");
        return EXIT_FAILURE;
    }
    struct pg_handle_t *pg = (struct pg_handle_t *) *pg_handle;

    if (setup_process_group_pd_and_qps(pg, ring_size) != 0) {
        fprintf(stderr, "Failed to setup process group PD and QPs\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    if (setup_process_group_local_info(pg) != 0) {
        fprintf(stderr, "Failed to setup process group local info\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    /* ---------------------------------------
     * Establish communication
     * --------------------------------------- */
    int sockfd_back = -1; // Socket file descriptors

    // Set the back socket to listen for incoming connections
    set_back_socket_to_listen(&sockfd_back);
    if (sockfd_back < 0) {
        fprintf(stderr, "Failed to set back socket to listen\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    // connect to front as client if you are in position 0
    if (ring_location==0 && exchange_rdma_information_front(pg, servername)==EXIT_FAILURE) {
        fprintf(stderr, "Failed to exchange RDMA information with front\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    if (exchange_rdma_information_back(pg, sockfd_back)==EXIT_FAILURE) {
        fprintf(stderr, "Failed to exchange RDMA information with back\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    // connect to front as client if you got a successful connection from back (not position 0)
    if (ring_location!=0 && exchange_rdma_information_front(pg, servername)==EXIT_FAILURE) {
        fprintf(stderr, "Failed to exchange RDMA information with front\n");
        pg_close(pg);
        return EXIT_FAILURE;
    }

    pg->ring_location = ring_location; // Placeholder - this should be assigned based on ring order
    pg->ring_size = ring_size; // Placeholder - this should be assigned based on total number of processes

    fprintf(stderr, "Reached end of connect_process_group\n");
    return EXIT_SUCCESS;
}

int exchange_registered_memory(struct pg_handle_t* pg) {
    char *self_msg = (char *)pg->send_mr->addr;
    char front_msg[2 * (sizeof(uint64_t) + sizeof(uint32_t))]; // For recvbuf and sendbuf

    // // for debug purposes, print the first bytes of the sendbuf
    // if (DEBUG_MODE) {
    //     fprintf(stdout, "sendbuf data at start: ");
    //     for (int i = 0; i < 16; i++) {
    //         fprintf(stdout, "%02x", ((char *)pg->send_mr->addr)[i]);
    //     }
    //     fprintf(stdout, "\n");
    // }

    // post receive for the address of the front server
    struct ibv_sge sge_recv = {
        .addr = (uint64_t) pg->recv_mr->addr,
        .length = sizeof(front_msg),
        .lkey = pg->recv_mr->lkey
    };
    struct ibv_recv_wr *bad_recv_wr, recv_wr = {
        .sg_list = &sge_recv,
        .num_sge = 1,
        .next = NULL
    };
    if (ibv_post_recv(pg->front.qp, &recv_wr, &bad_recv_wr)) {
        perror( "Error posting recv address exchange:");
        return EXIT_FAILURE;
    }

    // temporarily store the old data for the exchange time
    char tmp_data[sizeof(front_msg)];
    memcpy(tmp_data, self_msg, sizeof(front_msg));

    // post send address backwards
    memcpy(self_msg, &pg->recv_mr->addr, sizeof(uint64_t));  // recvbuf addr
    memcpy(self_msg + sizeof(uint64_t), &pg->recv_mr->lkey, sizeof(uint32_t)); // recvbuf key
    memcpy(self_msg + sizeof(uint64_t) + sizeof(uint32_t), &pg->send_mr->addr, sizeof(uint64_t));  // sendbuf addr
    memcpy(self_msg + 2 * sizeof(uint64_t) + sizeof(uint32_t), &pg->send_mr->lkey, sizeof(uint32_t)); // sendbuf key

    struct ibv_sge sge_send = {
        .addr = (uint64_t) pg->send_mr->addr,
        .length = sizeof(front_msg),
        .lkey = pg->send_mr->lkey
    };
    struct ibv_send_wr *bad_send_wr, send_wr = {
        .wr_id = 1,
        .sg_list = &sge_send,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED
    };
    if (ibv_post_send(pg->back.qp, &send_wr, &bad_send_wr)) {
        perror("Error posting send address exchange");
        return EXIT_FAILURE;
    }

    // Wait for send completion
    struct ibv_wc wc;
    while (ibv_poll_cq(pg->back.qp->send_cq, 1, &wc) == 0){};
    if (wc.status != IBV_WC_SUCCESS ) {
        fprintf(stderr, "Error in send completion: %s\n", ibv_wc_status_str(wc.status));
        return EXIT_FAILURE;
    }

    // Wait for receive completion
    while (ibv_poll_cq(pg->front.qp->recv_cq, 1, &wc) == 0){};
    if (wc.status != IBV_WC_SUCCESS ) {
        fprintf(stderr, "Error in receive completion: %s\n", ibv_wc_status_str(wc.status));
        return EXIT_FAILURE;
    }

    // Extract recvbuf details
    memcpy(&pg->front_credentials.recvbuf_addr, pg->recv_mr->addr, sizeof(uint64_t));
    memcpy(&pg->front_credentials.recvbuf_rkey, pg->recv_mr->addr + sizeof(uint64_t), sizeof(uint32_t));

    // Extract sendbuf details
    memcpy(&pg->front_credentials.sendbuf_addr, pg->recv_mr->addr + sizeof(uint64_t) + sizeof(uint32_t), sizeof(uint64_t));
    memcpy(&pg->front_credentials.sendbuf_rkey, pg->recv_mr->addr + 2 * sizeof(uint64_t) + sizeof(uint32_t), sizeof(uint32_t));

    if (DEBUG_MODE) {
        fprintf(stdout, "Received RDMA info from server:\n\trecvbuf_addr=0x%lx, recvbuf_rkey=0x%x,\n\tsendbuf_addr=0x%lx, sendbuf_rkey=0x%x\n",
                pg->front_credentials.recvbuf_addr, pg->front_credentials.recvbuf_rkey,
                pg->front_credentials.sendbuf_addr, pg->front_credentials.sendbuf_rkey);
        fprintf(stdout, "Sent RDMA info to server:\n\trecvbuf_addr=0x%lx, recvbuf_rkey=0x%x,\n\tsendbuf_addr=0x%lx, sendbuf_rkey=0x%x\n",
                pg->recv_mr->addr, pg->recv_mr->lkey, pg->send_mr->addr, pg->send_mr->lkey);
    }


    // deposit the original data back
    memcpy(self_msg, tmp_data, sizeof(front_msg));
    // trying to handle compiler optimization side effect
    return null_func(self_msg);

    // // for debug purposes, print the first bytes of the sendbuf
    // if (DEBUG_MODE) {
    //     fprintf(stdout, "sendbuf data at end: ");
    //     for (int i = 0; i < 16; i++) {
    //         fprintf(stdout, "%02x", ((char *)pg->send_mr->addr)[i]);
    //     }
    //     fprintf(stdout, "\n");
    // }

    // return EXIT_SUCCESS;
}

int register_memory(void* data, const DATATYPE datatype, const int count, struct pg_handle_t* pg_handle, void** sendbuf, void** recvbuf) {
    // Get the size of each data element based on the datatype
    const size_t elem_size = get_datatype_size(datatype);
    if (elem_size == EXIT_FAILURE) {
        perror("Unsupported data type");
        return EXIT_FAILURE;
    }

    // Register the sendbuf (points to data array)
    *sendbuf = data;
    pg_handle->send_mr = ibv_reg_mr(pg_handle->pd, *sendbuf, count * elem_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    if (!pg_handle->send_mr) {
        perror("Failed to register sendbuf memory region");
        return EXIT_FAILURE;
    }

    // Allocate and register the recvbuf
    const size_t page_size = sysconf(_SC_PAGESIZE);
    const size_t aligned_size = roundup(count * elem_size, page_size);

    if (posix_memalign(recvbuf, page_size, aligned_size) != 0) {
        ibv_dereg_mr(pg_handle->send_mr);
        perror("Failed to allocate aligned recvbuf");
        return EXIT_FAILURE;
    }

    pg_handle->recv_mr = ibv_reg_mr(pg_handle->pd, *recvbuf, aligned_size, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    if (!pg_handle->recv_mr) {
        free(*recvbuf);
        ibv_dereg_mr(pg_handle->send_mr);
        perror("Failed to register recvbuf memory region");
        return EXIT_FAILURE;
    }

    if (DEBUG_MODE) {
        fprintf(stdout, "sendbuf: %p, recvbuf: %p\n", *sendbuf, *recvbuf);
    }

    return EXIT_SUCCESS;
}


int unregister_memory(struct pg_handle_t* pg_handle, void** sendbuf, void** recvbuf) {
    // Deregister the memory region for sendbuf
    if (ibv_dereg_mr(pg_handle->send_mr)) {
        perror("Failed to deregister sendbuf memory region");
        return EXIT_FAILURE;
    }
    pg_handle->send_mr = NULL;

    // Deregister the memory region for recvbuf
    if (ibv_dereg_mr(pg_handle->recv_mr)) {
        perror("Failed to deregister recvbuf memory region");
        return EXIT_FAILURE;
    }
    pg_handle->recv_mr = NULL;

    // Free the memory allocated for recvbuf, and nullify both recvbuf and sendbuf pointers
    free(*recvbuf);
    *recvbuf = NULL;
    *sendbuf = NULL;

    return EXIT_SUCCESS;
}