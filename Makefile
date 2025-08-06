CC = gcc
CFLAGS = -Wall -Wextra -std=c23
LDFLAGS = -libverbs

# Source files
SOURCES = pg_main.c rdma_utils.c tcp_exchange.c ring_allreduce.c
HEADERS = pg_handle.h rdma_utils.h tcp_exchange.h ring_allreduce.h
OBJECTS = $(SOURCES:.c=.o)

# Target executable (example/test program)
TARGET = test_pg
TEST_SOURCE = test_program.c

# Default target
all: $(TARGET)

# Build the test program
$(TARGET): $(OBJECTS) $(TEST_SOURCE)
	$(CC) $(CFLAGS) -o $@ $(TEST_SOURCE) $(OBJECTS) $(LDFLAGS)

# Build object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Rebuild everything
rebuild: clean all

# Install dependencies info (for reference)
deps:
	@echo "Dependencies required:"
	@echo "  - libibverbs-dev (Ubuntu/Debian) or libibverbs-devel (RHEL/CentOS)"
	@echo "  - RDMA/InfiniBand hardware or software simulation"

.PHONY: all clean rebuild deps