// udp_client.c
#include <arpa/inet.h>
#include <errno.h>
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
        "Usage: %s SERVER_IP PORT [COUNT=%d] [PAYLOAD=%d] [TIMEOUT_MS=%d]\n"
        "  PAYLOAD must be >= %zu bytes\n",
        p, DEFAULT_COUNT, DEFAULT_PAYLOAD, DEFAULT_TIMEOUTMS,
        1 + sizeof(struct Meta));
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    long count   = (argc >= 4) ? atol(argv[3]) : DEFAULT_COUNT;
    long payload = (argc >= 5) ? atol(argv[4]) : DEFAULT_PAYLOAD;
    long to_ms   = (argc >= 6) ? atol(argv[5]) : DEFAULT_TIMEOUTMS;

    if (count <= 0)   { fprintf(stderr, "COUNT must be > 0\n"); return 1; }
    if (payload < (long)(1 + (long)sizeof(struct Meta))) {
        fprintf(stderr, "PAYLOAD must be >= %zu\n", 1 + sizeof(struct Meta));
        return 1;
    }
    if (to_ms < 1) to_ms = 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct timeval tv = { .tv_sec = to_ms / 1000, .tv_usec = (to_ms % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", server_ip);
        close(fd);
        return 1;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)payload);
    unsigned char *rxb = (unsigned char *)malloc((size_t)payload);
    if (!buf || !rxb) { perror("malloc"); free(buf); free(rxb); close(fd); return 1; }

    memset(buf, 0xAB, (size_t)payload);
    buf[0] = 0x42; // server will increment this

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
        // Exact requested format:
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
