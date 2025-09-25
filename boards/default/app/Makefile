CC      = gcc
CFLAGS  = -Wall -O2
TARGETS = udp_server udp_client file_sender file_receiver

all: $(TARGETS)

udp_server: udp_server.c
	$(CC) $(CFLAGS) -o $@ $<

udp_client: udp_client.c
	$(CC) $(CFLAGS) -o $@ $<

file_sender: file_sender.c
	$(CC) $(CFLAGS) -o $@ $<

file_receiver: file_receiver.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
