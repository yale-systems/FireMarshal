# Makefile for RDMA samples: client_ts, server_ts, client, server

# Compiler & flags
CC      ?= gcc
CSTD    ?= gnu11
CFLAGS  ?= -O2 -Wall -Wextra -std=$(CSTD)
LDFLAGS ?=
LDLIBS  ?= -lrdmacm -libverbs -pthread

# Enable with: make DEBUG=1
ifeq ($(DEBUG),1)
  CFLAGS += -O0 -g -DDEBUG
endif

# Binaries
BINS := client_ts client server server_ts

.PHONY: all clean
all: $(BINS)

# Generic compile rule
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link rules
client_ts: client_ts.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

server_ts: server_ts.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

client: client.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

server: server.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(BINS) *.o
