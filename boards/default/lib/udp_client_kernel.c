#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP   "10.0.0.2"
#define SERVER_PORT 1111
#define DEFAULT_NTEST 64
#define DEFAULT_PAYLOAD 64
#define MAX_PAYLOAD 1472  // keep under MTU; adjust if needed

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--ntest N] [--payload-size BYTES]\n"
        "Defaults: --ntest %d, --payload-size %d\n",
        prog, DEFAULT_NTEST, DEFAULT_PAYLOAD);
}

static long long ts_diff_ns(const struct timespec *a, const struct timespec *b) {
    // return (a - b) in nanoseconds
    long long s  = (long long)a->tv_sec  - (long long)b->tv_sec;
    long long ns = (long long)a->tv_nsec - (long long)b->tv_nsec;
    return s*1000000000LL + ns;
}

int main(int argc, char **argv) {
    int ntest = DEFAULT_NTEST;
    int payload = DEFAULT_PAYLOAD;

    // Simple arg parsing: --ntest N and --payload-size BYTES
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ntest") == 0 && i+1 < argc) {
            ntest = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--ntest=", 8) == 0) {
            ntest = atoi(argv[i] + 8);
        } else if (strcmp(argv[i], "--payload-size") == 0 && i+1 < argc) {
            payload = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--payload-size=", 15) == 0) {
            payload = atoi(argv[i] + 15);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (ntest <= 0) {
        fprintf(stderr, "ntest must be > 0\n");
        return 1;
    }
    if (payload <= 0 || payload > MAX_PAYLOAD) {
        fprintf(stderr, "payload-size must be between 1 and %d\n", MAX_PAYLOAD);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SERVER_PORT);
    if (inet_aton(SERVER_IP, &dst.sin_addr) == 0) {
        fprintf(stderr, "Invalid server IP\n");
        return 1;
    }

    uint8_t *buf = malloc(payload);
    if (!buf) { perror("malloc"); return 1; }

    printf("UDP client -> %s:%d | ntest=%d payload=%d\n",
           SERVER_IP, SERVER_PORT, ntest, payload);

    long long sum_ns = 0, min_ns = 0, max_ns = 0;
    int received_ok = 0;

    for (int i = 0; i < ntest; i++) {
        // Prepare payload: first byte changes each test, rest zeros
        memset(buf, 0, payload);
        buf[0] = (uint8_t)(i & 0xFF);

        struct timespec t0, t1;
        if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) { perror("clock_gettime"); break; }

        ssize_t sent = sendto(sock, buf, payload, 0,
                              (struct sockaddr *)&dst, sizeof(dst));
        if (sent != payload) {
            if (sent < 0) perror("sendto");
            else fprintf(stderr, "Partial send: %zd/%d\n", sent, payload);
            continue;
        }

        // Receive reply
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, payload, 0,
                             (struct sockaddr *)&src, &slen);
        if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) { perror("clock_gettime"); break; }

        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "Timeout waiting for reply (iter %d)\n", i);
            } else {
                perror("recvfrom");
            }
            continue;
        }

        if (n != payload) {
            fprintf(stderr, "Unexpected reply size %zd (expected %d)\n", n, payload);
            continue;
        }

        // Optional: verify the source is the server
        if (src.sin_addr.s_addr != dst.sin_addr.s_addr || src.sin_port != dst.sin_port) {
            char sip[32]; inet_ntop(AF_INET, &src.sin_addr, sip, sizeof(sip));
            fprintf(stderr, "Ignoring packet from %s:%d\n",
                    sip, ntohs(src.sin_port));
            continue;
        }

        long long rtt_ns = ts_diff_ns(&t1, &t0);
        if (received_ok == 0) {
            min_ns = max_ns = rtt_ns;
        } else {
            if (rtt_ns < min_ns) min_ns = rtt_ns;
            if (rtt_ns > max_ns) max_ns = rtt_ns;
        }
        sum_ns += rtt_ns;
        received_ok++;

        printf("iter=%d rtt=%.3f ms\n", i, rtt_ns / 1e6);
    }

    if (received_ok > 0) {
        double avg_ms = (sum_ns / (double)received_ok) / 1e6;
        printf("\nResults: recv=%d/%d  min=%.3f ms  avg=%.3f ms  max=%.3f ms\n",
               received_ok, ntest, min_ns/1e6, avg_ms, max_ns/1e6);
    } else {
        printf("\nNo replies received.\n");
    }

    free(buf);
    close(sock);
    return 0;
}
