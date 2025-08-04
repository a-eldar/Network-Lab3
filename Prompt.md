
We are making a **C** library for **All Reduce** using the **Ring** algorithm, over **RDMA**.

In order to retain information between the functions, we need a `struct PGHandle` that will hold all the relevant data for communication between the machines. Since we are implementing Ring, we need to hold communication between each server, and both the one on its left in the list, and the one on its right in the list.

Implement the `PGHandle` struct as required.

# Library Functions
We need to implement 3 functions in our library.

> [!Notice]
> Make sure you implement them in a modular way, and use helper functions to make it more readable as well.

## Connect Process Group
```c
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
```
- `serverlist` - a list of server names. The IP will be acquired using the server name.
- `len` - the length of the `serverlist`.
- `idx` - the index of the current server in the list.
- `pg_handle` - an empty `PGHandle` struct to fill with the relevant information.
- Returns: 0 on success, -1 on failure

This function connects each of the servers to their two neighbors in the list over RDMA. Each server should write to the server on its right and read from the output of the server on its left. Remember that in RDMA we use one-sided **write**.
In order to establish the connection, each server should send the relevant information to the neighbor on its left, using TCP, in order for it to know where to write to and establish its connection.

In TCP part we need the first server (index 0) to send the relevant information to its left, and then listen for incoming information from the its right. The rest of the servers should do the opposite, first listening and then sending. This is done in order to prevent a deadlock.

## All Reduce
```c
typedef enum {
	INT, DOUBLE
} DATATYPE;

typedef enum {
	SUM, MULT
} OPERATION;

int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
```
- `sendbuf` - input buffer to the Ring algorithm
- `recvbuf` - output buffer
- `count` - number of elements
- `datatype` - datatype of elements
- `op` - operation to perform in the algorithm
- `pg_handle` - filled by the `connect_process_group` function.
- Returns: 0 on success, -1 on failure.

Preforms the **Ring** algorithm.
If deemed necessary, you can force them to synchronize occasionally.

## PG Close
```c
int pg_close(PGHandle* pg_handle);
```
- `pg_handle` - filled by the `connect_process_group` function.

Closes the connection and frees all the memory allocated by `connect_process_group`.
# Algorithms
## Ring Algorithm
**Ring all-reduce**
Suppose each user has an input vector, and we want to calculate the sum of the vectors, and return the result to all users.

We start by having each user partition the vector into the $n$ - the number of users.
Then each user takes a different section of the vector - so user 1 takes the first section, user 2 takes the second etc.
### First Ring
Now we calculate the sum:
Each user passes the selected section around to the next process in a ring form.
With each pass, the user adds their corresponding part to the received section.
Then they pass that to the next user etc.
In the end of this ring, we'll have each section of the vector sum, scattered among the users.
### Second Ring
Now we return the result to everyone:
Each user has a section of the result vector, so they copy that section for themselves, then pass it to the next user.
In the end of this ring, each user will have the result vector sum.