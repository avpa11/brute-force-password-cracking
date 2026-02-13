CC = gcc
CFLAGS = -Wall -Wextra -O2 -I/opt/homebrew/opt/libxcrypt/include
LDFLAGS = -L/opt/homebrew/opt/libxcrypt/lib -lcrypt

all: controller worker

controller: controller.c header.h
	$(CC) $(CFLAGS) -o controller controller.c

worker: worker.c header.h
	$(CC) $(CFLAGS) -o worker worker.c $(LDFLAGS)

clean:
	rm -f controller worker

.PHONY: all clean