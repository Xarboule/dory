all: test

CC=gcc
CFLAGS=-O0 -Wall -g

LIBS=-libverbs -lrdmacm -lpthread

test: test.o
	$(CC) $(CFLAGS) test.o bypass.h -o test $(LIBS)

clean:
	rm -rf *.o test *~
