CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS =

TARGETS = server client

.PHONY: all clean

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)
