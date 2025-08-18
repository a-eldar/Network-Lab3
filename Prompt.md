# RDMA Ring Collectives Library
We want to implement a **C library** for collective communication over **RDMA** using the **Ring algorithm**. This library should provide **reduce-scatter, all-gather, and all-reduce** operations.

The code will run in a **real RDMA-capable cluster** with multiple machines. Each machine will run the same program, but with a different index to identify its position in the process group.

The LLM should generate modular, readable C code with proper headers, helper functions, and a simple Makefile using `libverbs`.

---

## Background

In collective communication, processes exchange data in a coordinated way:

- **Reduce-Scatter**: Each process contributes data, which is reduced (e.g., summed), and each process ends up with a distinct part of the reduced result.
    
- **All-Gather**: Each process starts with a unique piece of data, and by the end, all processes have all the data.
    
- **All-Reduce**: A combination of reduce-scatter followed by all-gather. Each process starts with input data, and by the end, all have the reduced result.
    

We implement these using the **Ring algorithm**:

- In the **first ring pass**, processes circulate partial results around the ring, accumulating contributions.
    
- In the **second ring pass**, the final results are distributed back so every process has the complete output.
    

We want to compare **Eager vs Rendezvous protocols**:

- **Eager** sends small messages immediately.
    
- **Rendezvous** coordinates before sending large messages, reducing unnecessary transfers.  
    Optimizations to include:
    
- **RDMA Write with Immediate** for signaling completion.
    
- **RDMA Read or RDMA Write with zero-copy** for large messages in the all-gather phase.
    
- **Pipelining** to overlap communication and computation.
    

---

## Library Requirements

Define a struct:

```c
typedef struct {
	// All relevant data for RDMA communication
	// QPs, CQs, buffers, connection info for left/right neighbors, etc.
} PGHandle;
```

### Functions

#### 1. Connect Process Group

```c
int connect_process_group(char** serverlist, int len, int idx, PGHandle* pg_handle);
```

- Connects each server to its **left and right neighbors** in the ring.
    
- Uses TCP sockets for initial exchange of connection info.
    
- Index `0` should first send then listen (to avoid deadlock); others first listen then send. Repeat in the opposite direction.
    
- Allow asynchronous connection by having index `0` resend its info repeatedly until neighbors succeed.
    

#### 2. Reduce-Scatter

```c
int pg_reduce_scatter(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
```
- Perform the reduce-scatter part of ring all-reduce.
    

#### 3. All-Gather

```c
int pg_all_gather(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, PGHandle* pg_handle);
```
- Perform the all-gather phase, using zero-copy techniques for large messages.
    

#### 4. All-Reduce

```c
int pg_all_reduce(void* sendbuf, void* recvbuf, int count, DATATYPE datatype, OPERATION op, PGHandle* pg_handle);
```
- Implement all-reduce as reduce-scatter + all-gather.
    
- Allow synchronization points if needed.
    

#### 5. Close Process Group

```c
int pg_close(PGHandle* pg_handle);
```

- Close connections and free all allocated resources.
    

---

## Datatypes and Operations

```c
typedef enum { INT, DOUBLE } DATATYPE;
typedef enum { SUM, MULT } OPERATION;
```  

---

## Testing

Write a **simple test program**:

- Takes `-myindex` (the process index) and `-list` (list of servers).
    
- Calls `connect_process_group`.
    
- Verifies RDMA connectivity by sending a test message from each server to its right neighbor and checking it arrives.
    
- Optionally runs a small all-reduce test with integers to confirm correctness.
    

---

## Debugging

- Add **debug print statements** throughout the code to trace execution.
    
- Guard them with a compile-time constant:
    

```c
#define DEBUG 1
...
if (DEBUG) printf("Connected to neighbor %d\n", right_neighbor);
```

When `DEBUG` is set to `0`, no debug prints should appear.

---

## Implementation Notes

- Separate the code into multiple `.c` and `.h` files.
    
- Write a **simple Makefile** to build the library and test program.
    
- Keep the implementation modular. Use helper functions to keep code clean and readable.
    
- Do not make it a static or shared library—just compile the object files into an executable for testing.
    

---

## Task

Implement:

1. `PGHandle` struct.
    
2. `connect_process_group`, `pg_reduce_scatter`, `pg_all_gather`, `pg_all_reduce`, and `pg_close`.
    
3. A simple test program for connectivity and correctness.