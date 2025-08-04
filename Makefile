# Makefile for RDMA Ring All-Reduce Library

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g
INCLUDES = -I.
LIBS = -libverbs -lrdmacm -lpthread

# Source files
SOURCES = main_api.c rdma_connection.c tcp_setup.c ring_algorithm.c utility.c
OBJECTS = $(SOURCES:.c=.o)
HEADER = rdma_allreduce.h

# Library name
LIBRARY = librdma_allreduce.a
SHARED_LIBRARY = librdma_allreduce.so

# Example program
EXAMPLE = example_allreduce
EXAMPLE_SRC = example.c

# Default target
all: $(LIBRARY) $(SHARED_LIBRARY) $(EXAMPLE)

# Static library
$(LIBRARY): $(OBJECTS)
	ar rcs $@ $^
	@echo "Static library created: $(LIBRARY)"

# Shared library
$(SHARED_LIBRARY): $(OBJECTS)
	$(CC) -shared -fPIC -o $@ $^ $(LIBS)
	@echo "Shared library created: $(SHARED_LIBRARY)"

# Object files
%.o: %.c $(HEADER)
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -c $< -o $@

# Example program
$(EXAMPLE): $(EXAMPLE_SRC) $(LIBRARY)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< -L. -lrdma_allreduce $(LIBS)
	@echo "Example program created: $(EXAMPLE)"

# Clean target
clean:
	rm -f $(OBJECTS) $(LIBRARY) $(SHARED_LIBRARY) $(EXAMPLE)
	@echo "Cleaned all build artifacts"

# Install target (optional)
install: $(LIBRARY) $(SHARED_LIBRARY)
	sudo cp $(LIBRARY) /usr/local/lib/
	sudo cp $(SHARED_LIBRARY) /usr/local/lib/
	sudo cp $(HEADER) /usr/local/include/
	sudo ldconfig
	@echo "Library installed to /usr/local/lib and /usr/local/include"

# Uninstall target (optional)
uninstall:
	sudo rm -f /usr/local/lib/$(LIBRARY)
	sudo rm -f /usr/local/lib/$(SHARED_LIBRARY)
	sudo rm -f /usr/local/include/$(HEADER)
	sudo ldconfig
	@echo "Library uninstalled"

# Debug target
debug: CFLAGS += -DDEBUG -g3
debug: all

# Test target
test: $(EXAMPLE)
	@echo "Running basic functionality test..."
	@echo "Note: This requires RDMA-capable hardware and multiple nodes"
	@echo "Usage: ./$(EXAMPLE) <server1> <server2> ... <serverN> <my_index>"

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build static library, shared library, and example"
	@echo "  clean     - Remove all build artifacts"
	@echo "  install   - Install library and headers to system directories"
	@echo "  uninstall - Remove library and headers from system directories"
	@echo "  debug     - Build with debug flags"
	@echo "  test      - Build and provide test instructions"
	@echo "  help      - Show this help message"

# Phony targets
.PHONY: all clean install uninstall debug test help

# Dependencies
main_api.o: main_api.c rdma_allreduce.h
rdma_connection.o: rdma_connection.c rdma_allreduce.h
tcp_setup.o: tcp_setup.c rdma_allreduce.h
ring_algorithm.o: ring_algorithm.c rdma_allreduce.h
utility.o: utility.c rdma_allreduce.h