CC = gcc
CFLAGS = -O2 -Wall -g
LIBS = -libverbs

all: test_ring

pg.o: pg.c pg.h
	$(CC) $(CFLAGS) -c pg.c -o pg.o

test_ring.o: test_ring.c pg.h
	$(CC) $(CFLAGS) -c test_ring.c -o test_ring.o

test_ring: pg.o test_ring.o
	$(CC) $(CFLAGS) pg.o test_ring.o -o test_ring -libverbs

clean:
	rm -f *.o test_ring
