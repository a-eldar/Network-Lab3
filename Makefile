CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=
LIBS = -libverbs

SRCS = pg_connect.c pg_collectives.c pg_allreduce.c pg_close.c
HDRS = pg_handle.h pg_connect.h pg_collectives.h pg_allreduce.h

OBJS = $(SRCS:.c=.o)

all: test_allreduce

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

test_allreduce: $(OBJS) test_allreduce.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

clean:
	rm -f *.o test_allreduce

.PHONY: all clean

