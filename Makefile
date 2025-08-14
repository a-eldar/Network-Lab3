CC = gcc
CFLAGS = -Wall -g -O2
LDFLAGS = -libverbs -lpthread

# Source files
SRCS = rdma_utils.c pg_connect.c pg_allreduce.c pg_close.c
OBJS = $(SRCS:.c=.o)

# Header files
HEADERS = pg_handle.h rdma_utils.h pg_allreduce.h

# Test program (optional)
TEST_SRC = test_allreduce.c
TEST_OBJ = $(TEST_SRC:.c=.o)
TEST_BIN = test_allreduce

# Default target - build object files only
all: $(OBJS)

# Compile source files to object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Build test program (optional)
test: $(OBJS) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(OBJS) $(TEST_OBJ) $(LDFLAGS)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TEST_OBJ) $(TEST_BIN)
# Install headers (optional)
install-headers:
	mkdir -p /usr/local/include/pg_allreduce
	cp $(HEADERS) /usr/local/include/pg_allreduce/

bw_make:
	gcc bw_template.c -libverbs -o server && ln -s server client

.PHONY: all clean test install-headers