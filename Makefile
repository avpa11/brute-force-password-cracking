CC = gcc
CFLAGS = -Wall -Wextra -O2 -I/opt/homebrew/opt/libxcrypt/include
LDFLAGS = -L/opt/homebrew/opt/libxcrypt/lib -lcrypt

all: controller worker gen_hash

util.o: util.c util.h
	$(CC) $(CFLAGS) -c -o util.o util.c

controller: controller.c header.h util.h util.o
	$(CC) $(CFLAGS) -o controller controller.c util.o

worker: worker.c header.h util.h util.o
	$(CC) $(CFLAGS) -o worker worker.c util.o $(LDFLAGS) -pthread

gen_hash: gen_hash.c
	$(CC) $(CFLAGS) -o gen_hash gen_hash.c $(LDFLAGS)

clean:
	rm -f controller worker gen_hash util.o

.PHONY: all clean
