// udp_client.c (long options version)
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#define DEFAULT_COUNT     1024
#define DEFAULT_PAYLOAD   1500     // match server buffer by default
#define DEFAULT_TIMEOUTMS 1000     // per-packet recv timeout

// Server increments buffer[0]; keep our metadata AFTER byte 0.
struct __attribute__((__packed__)) Meta {
    uint32_t seq;       // sequence number
    uint64_t send_ns;   // send timestamp (ns)
};

static inline uint64_t ts2ns(struct timespec t) {
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --server IP --dst-port PORT [--count N] [--payload BYTES] [--timeout MS]\n"
        "     [--src-port PORT] [--src-ip IP]\n\n"
        "Options:\n"
        "  --server,   -S  IPv4 address of the server (required)\n"
        "  --dst-port, -P  UDP destination port on server (required)\n"
        "  --count,    -n  Number of messages to send (default %d)\n"
        "  --payload,  -B  Bytes per message (>= %zu, default %d)\n"
        "  --timeout,  -t  Per-packet recv timeout in ms (default %d)\n"
        "  --src-port, -s  Bind client to this local UDP port (optional)\n"
        "  --src-ip,   -I  Bind client to this local IPv4 (optional)\n"
        "  --help,     -h  Show this help\n",
        p, DEFAULT_COUNT, 1 + sizeof(struct Meta), DEFAULT_PAYLOAD, DEFAULT_TIMEOUTMS);
}

int main(int argc, char **argv) {
    // Defaults
    char server_ip[INET_ADDRSTRLEN] = {0};
    int  dst_port   = 0;
    long count      = DEFAULT_COUNT;
    long payload    = DEFAULT_PAYLOAD;
    long to_ms      = DEFAULT_TIMEOUTMS;
    int  src_port   = 0;
    char src_ip[INET_ADDRSTRLEN] = {0};  // optional bind IP

    static struct option longopts[] = {
        {"server",   required_argument, 0, 'S'},
        {"dst-port", required_argument, 0, 'P'},
        {"count",    required_argument, 0, 'n'},
        {"payload",  required_argument, 0, 'B'},
        {"timeout",  required_argument, 0, 't'},
        {"src-port", required_argument, 0, 's'},
        {"src-ip",   required_argument, 0, 'I'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "S:P:n:B:t:s:I:h", longopts, &idx)) != -1) {
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
                if (v <= 0) { fprintf(stderr, "Invalid --count\n"); return 1; }
                count = v;
                break;
            }
            case 'B': {
                long v = strtol(optarg, NULL, 10);
                if (v <= 0) { fprintf(stderr, "Invalid --payload\n"); return 1; }
                payload = v;
                break;
            }
            case 't': {
                long v = strtol(optarg, NULL, 10);
                if (v < 1) { fprintf(stderr, "Invalid --timeout\n"); return 1; }
                to_ms = v;
                break;
            }
            case 's': {
                long v = strtol(optarg, NULL, 10);
                if (v < 0 || v > 65535) { fprintf(stderr, "Invalid --src-port\n"); return 1; }
                src_port = (int)v;
                break;
            }
            case 'I':
                strncpy(src_ip, optarg, sizeof(src_ip)-1);
                src_ip[sizeof(src_ip)-1] = '\0';
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (server_ip[0] == '\0' || dst_port == 0) {
        usage(argv[0]);
        return 1;
    }
    if (payload < (long)(1 + (long)sizeof(struct Meta))) {
        fprintf(stderr, "PAYLOAD must be >= %zu\n", 1 + sizeof(struct Meta));
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    // Optional: bind to specific local IP/port
    if (src_port != 0 || src_ip[0] != '\0') {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        #ifdef SO_REUSEPORT
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        #endif

        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        src.sin_port   = htons((uint16_t)src_port);
        if (src_ip[0] == '\0') {
            src.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if (inet_pton(AF_INET, src_ip, &src.sin_addr) != 1) {
                fprintf(stderr, "Invalid --src-ip: %s\n", src_ip);
                close(fd);
                return 1;
            }
        }
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
            perror("bind (source)");
            close(fd);
            return 1;
        }
    }

    // Set recv timeout
    struct timeval tv = { .tv_sec = to_ms / 1000, .tv_usec = (to_ms % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Destination
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "Invalid --server IP: %s\n", server_ip);
        close(fd);
        return 1;
    }

    // Buffers
    unsigned char *buf = (unsigned char *)malloc((size_t)payload);
    unsigned char *rxb = (unsigned char *)malloc((size_t)payload);
    if (!buf || !rxb) { perror("malloc"); free(buf); free(rxb); close(fd); return 1; }

    memset(buf, 0xAB, (size_t)payload);
    buf[0] = 0x42; // server will increment this

    // Stats
    uint64_t min_ns = UINT64_MAX, max_ns = 0, sum_ns = 0;
    long received = 0;

    for (long i = 0; i < count; i++) {
        struct Meta *m = (struct Meta *)(buf + 1);
        m->seq = (uint32_t)i;

        struct timespec ts_send;
        if (clock_gettime(CLOCK_MONOTONIC, &ts_send) != 0) {
            perror("clock_gettime(send)");
            continue;
        }
        m->send_ns = ts2ns(ts_send);

        if (sendto(fd, buf, (size_t)payload, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            // send error; skip
            continue;
        }

        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        ssize_t r = recvfrom(fd, rxb, (size_t)payload, 0, (struct sockaddr *)&src, &sl);
        if (r < 0) {
            // timeout or error; treat as loss
            continue;
        }

        struct timespec ts_recv;
        if (clock_gettime(CLOCK_MONOTONIC, &ts_recv) != 0) {
            continue;
        }
        uint64_t recv_ns = ts2ns(ts_recv);

        if (r < (ssize_t)(1 + sizeof(struct Meta))) {
            // too short; skip
            continue;
        }

        const struct Meta *rm = (const struct Meta *)(rxb + 1);
        (void)rm; // seq available if you want to check order

        uint64_t rtt = recv_ns - rm->send_ns;
        if (rtt < min_ns) min_ns = rtt;
        if (rtt > max_ns) max_ns = rtt;
        sum_ns += rtt;
        received++;
    }

    if (received > 0) {
        double min_us = (double)min_ns / 1000.0;
        double avg_us = (double)sum_ns / (double)received / 1000.0;
        double max_us = (double)max_ns / 1000.0;
        printf("Results (block): recv=%ld/%ld  min=%.2f us , avg=%.2f us , max=%.2f us\n",
               received, count, min_us, avg_us, max_us);
    } else {
        printf("Results (block): recv=0/%ld  min=NA , avg=NA , max=NA\n", count);
    }

    free(buf);
    free(rxb);
    close(fd);
    return 0;
}
