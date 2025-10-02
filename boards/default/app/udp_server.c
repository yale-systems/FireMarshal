// udp_server.c
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_ADDR "0.0.0.0"   // listen on all interfaces by default
#define DEFAULT_PORT 1111
#define BUF_SIZE     1500        // Typical MTU; bump if you expect larger packets

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [BIND_IP [PORT]]\n"
            "  BIND_IP : IPv4 address to bind (default %s)\n"
            "  PORT    : UDP port number (default %d)\n"
            "Examples:\n"
            "  %s                # listen on 0.0.0.0:%d (all interfaces)\n"
            "  %s 10.0.0.2       # listen on 10.0.0.2:%d\n"
            "  %s 10.0.0.2 2222  # listen on 10.0.0.2:2222\n",
            prog, DEFAULT_ADDR, DEFAULT_PORT,
            prog, DEFAULT_PORT,
            prog, DEFAULT_PORT,
            prog);
}

int main(int argc, char **argv) {
    const char *bind_ip = DEFAULT_ADDR;
    int port = DEFAULT_PORT;

    if (argc >= 2) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            return 0;
        }
        bind_ip = argv[1];
    }
    if (argc >= 3) {
        char *end = NULL;
        long p = strtol(argv[2], &end, 10);
        if (!argv[2][0] || (end && *end) || p < 1 || p > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            usage(argv[0]);
            return 1;
        }
        port = (int)p;
    }
    if (argc > 3) {
        usage(argv[0]);
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    // Allow quick restarts
    int one = 1;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((uint16_t)port);

    if (strcmp(bind_ip, "0.0.0.0") == 0 || strcmp(bind_ip, "*") == 0) {
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, bind_ip, &server_addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid IPv4 address: %s\n", bind_ip);
            close(sockfd);
            return 1;
        }
    }

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    printf("UDP server listening on %s:%d\n", bind_ip, port);

    unsigned char buffer[BUF_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    for (;;) {
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            // Interrupted by signal? keep going.
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }

        if (n > 0) {
            buffer[0] = (unsigned char)((buffer[0] + 1) & 0xFF);  // simple mutation
        }
	// usleep(1000);
	//printf("received\n");
        if (sendto(sockfd, buffer, (size_t)n, 0,
                   (struct sockaddr *)&client_addr, addr_len) < 0) {
            perror("sendto");
        }
    }

    // Not reached
    close(sockfd);
    return 0;
}
