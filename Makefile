# Simple Makefile for RDMA Ring All-Reduce

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
LIBS = -libverbs

# Source files
SOURCES = main_api.c rdma_connection.c tcp_setup.c ring_algorithm.c utility.c
HEADER = rdma_allreduce.h

# Example program
EXAMPLE = example_allreduce
EXAMPLE_SRC = example.c

# Default target
all: $(EXAMPLE)

# Build example program with all source files
$(EXAMPLE): $(SOURCES) $(EXAMPLE_SRC) $(HEADER)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(EXAMPLE_SRC) $(LIBS)
	@echo "Example program created: $(EXAMPLE)"

# Clean target
clean:
	rm -f $(EXAMPLE)
	@echo "Cleaned build artifacts"

# Help target
help:
	@echo "Available targets:"
	@echo "  all   - Build example program"
	@echo "  clean - Remove build artifacts"
	@echo "  help  - Show this help message"

# Test run instructions
test: $(EXAMPLE)
	@echo "Program built successfully!"
	@echo "Usage: ./$(EXAMPLE) <server1> <server2> ... <serverN> <my_index>"
	@echo "Example: ./$(EXAMPLE) node1 node2 node3 0"

.PHONY: all clean help test