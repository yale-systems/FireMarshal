// udp_client_min.c — send N UDP datagrams of size BYTES, then exit.
// Adds optional --src-port to bind the local source port.
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --server IP --dst-port PORT --ntest N --bytes BYTES [--src-port PORT]\n"
        "Options:\n"
        "  --server,    -S   IPv4 address of the server (required)\n"
        "  --dst-port,  -P   UDP destination port on server (required)\n"
        "  --ntest,     -n   Number of datagrams to send (required)\n"
        "  --bytes,     -B   Bytes per datagram (required)\n"
        "  --src-port,  -s   Bind client to this local UDP port (optional)\n"
        "  --help,      -h   Show this help\n", p);
}

int main(int argc, char **argv) {
    char server_ip[INET_ADDRSTRLEN] = {0};
    int  dst_port = 0;
    long ntest = 0;
    long bytes = 0;
    int  src_port = 0;  // optional

    static struct option longopts[] = {
        {"server",   required_argument, 0, 'S'},
        {"dst-port", required_argument, 0, 'P'},
        {"ntest",    required_argument, 0, 'n'},
        {"bytes",    required_argument, 0, 'B'},
        {"src-port", required_argument, 0, 's'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "S:P:n:B:s:h", longopts, &idx)) != -1) {
        switch (opt) {
            case 'S':
                strncpy(server_ip, optarg, sizeof(server_ip)-1);
                server_ip[sizeof(server_ip)-1] = '\0';
                break;
            case 'P': {
                long p = strtol(optarg, NULL, 10);
                if (p < 1 || p > 65535) { fprintf(stderr, "Invalid --dst-port\n"); return 1; }
                dst_port = (int)p;
                break;
            }
            case 'n': {
                long v = strtol(optarg, NULL, 10);
                if (v <= 0) { fprintf(stderr, "Invalid --ntest\n"); return 1; }
                ntest = v;
                break;
            }
            case 'B': {
                long v = strtol(optarg, NULL, 10);
                if (v <= 0) { fprintf(stderr, "Invalid --bytes\n"); return 1; }
                bytes = v;
                break;
            }
            case 's': {
                long v = strtol(optarg, NULL, 10);
                if (v < 1 || v > 65535) { fprintf(stderr, "Invalid --src-port\n"); return 1; }
                src_port = (int)v;
                break;
            }
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (server_ip[0] == '\0' || dst_port == 0 || ntest <= 0 || bytes <= 0) {
        usage(argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    // Optional: bind to specific local port (INADDR_ANY)
    if (src_port != 0) {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        #ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        #endif
        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        src.sin_port   = htons((uint16_t)src_port);
        src.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
            perror("bind (source)");
            close(fd);
            return 1;
        }
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "Invalid --server IP: %s\n", server_ip);
        close(fd);
        return 1;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)bytes);
    if (!buf) { perror("malloc"); close(fd); return 1; }
    memset(buf, 0xAB, (size_t)bytes);  // arbitrary payload

    for (long i = 0; i < ntest; i++) {
        ssize_t s = sendto(fd, buf, (size_t)bytes, 0, (struct sockaddr *)&dst, sizeof(dst));
        if (s < 0) { perror("sendto"); break; }
    }

    free(buf);
    close(fd);
    return 0;
}
