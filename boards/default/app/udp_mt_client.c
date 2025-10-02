// udp_mt_client.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// ---------- CLI defaults ----------
#define DEF_THREADS     64
#define DEF_COUNT       1024
#define DEF_SIZE        1024
#define DEF_BASE_PORT   1200
#define DEF_TIMEOUT_MS  1000
#define DEF_OUTFILE     "udp_results.csv"

// ---------- per-thread context ----------
struct thread_ctx {
    int             tidx;
    char            server[64];
    uint16_t        dport;
    int             count;
    size_t          size;
    int             timeout_ms;

    long long      *rtt_us;      // per request; -1 on timeout
    int             ok;
    int             timeouts;
};

static void print_per_connection_avg(struct thread_ctx *ctx, int nthreads, int count) {
    printf("Per-connection average RTTs:\n");
    for (int i = 0; i < nthreads; i++) {
        long long sum_ns = 0;
        int ok = 0;
        for (int k = 0; k < count; k++) {
            long long ns = ctx[i].rtt_us[k];    // stored in ns; -1 means timeout
            if (ns >= 0) { sum_ns += ns; ok++; }
        }
        if (ok > 0) {
            double avg_us = (double)sum_ns / (double)ok / 1000.0;  // ns -> µs
            printf("  port %u: avg = %.1f us  (ok=%d/%d, timeouts=%d)\n",
                   (unsigned)ctx[i].dport, avg_us, ok, count, count - ok);
        } else {
            printf("  port %u: avg = NA (no responses)\n", (unsigned)ctx[i].dport);
        }
    }
}

static inline long long tsdiff_ns(struct timespec a, struct timespec b) {
    long long sec  = (long long)b.tv_sec  - (long long)a.tv_sec;
    long long nsec = (long long)b.tv_nsec - (long long)a.tv_nsec;
    return sec * 1000000000LL + nsec; // total ns
}

#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

// Enable NIC-level HW timestamping on an interface (requires CAP_NET_ADMIN)
static int enable_hw_ts_on_iface(const char *ifname, int fd) {
    struct ifreq ifr;
    struct hwtstamp_config cfg;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = 0;
    cfg.tx_type = HWTSTAMP_TX_ON;
    cfg.rx_filter = HWTSTAMP_FILTER_ALL;

    ifr.ifr_data = (void *)&cfg;
    if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
        perror("SIOCSHWTSTAMP");
        return -1;
    }
    return 0;
}

// Enable per-socket timestamping (HW RX & TX; also request RAW/HW clock values)
static int enable_sock_ts(int fd) {
    int val =
        SOF_TIMESTAMPING_TX_HARDWARE   |
        SOF_TIMESTAMPING_RX_HARDWARE   |
        SOF_TIMESTAMPING_RAW_HARDWARE  | // get PHC/raw if driver provides
        SOF_TIMESTAMPING_SOFTWARE;       // fallback, also helps testing

    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(val)) < 0) {
        perror("setsockopt(SO_TIMESTAMPING)");
        return -1;
    }
    return 0;
}

static void *thread_main(void *arg) {
    struct thread_ctx *ctx = (struct thread_ctx *)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        for (int i = 0; i < ctx->count; i++) ctx->rtt_us[i] = -1;
        return NULL;
    }

    struct timeval tv;
    tv.tv_sec  = ctx->timeout_ms / 1000;
    tv.tv_usec = (ctx->timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(ctx->dport);
    if (inet_pton(AF_INET, ctx->server, &dst.sin_addr) != 1) {
        fprintf(stderr, "[t%02d] bad server IP: %s\n", ctx->tidx, ctx->server);
        for (int i = 0; i < ctx->count; i++) ctx->rtt_us[i] = -1;
        close(fd);
        return NULL;
    }

    int one = 1;
    // Not strictly required here (ports are unique), but harmless and can help if something lingers
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in src = {0};
    src.sin_family      = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_ANY);      // bind to all local ifaces
    src.sin_port        = htons(ctx->dport);      // source port == destination port

    if (bind(fd, (struct sockaddr*)&src, sizeof(src)) < 0) {
        perror("bind (source port == dest port)");
        // mark this thread's requests as failed and exit gracefully
        for (int i = 0; i < ctx->count; i++) ctx->rtt_us[i] = -1;
        close(fd);
        return NULL;
    }

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("connect");
        for (int i = 0; i < ctx->count; i++) ctx->rtt_us[i] = -1;
        close(fd);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(ctx->size);
    if (!buf) {
        perror("malloc payload");
        for (int i = 0; i < ctx->count; i++) ctx->rtt_us[i] = -1;
        close(fd);
        return NULL;
    }
    for (size_t i = 0; i < ctx->size; i++) buf[i] = (uint8_t)(i & 0xff);

    ctx->ok = 0;
    ctx->timeouts = 0;

    for (int i = 0; i < ctx->count; i++) {
        if (ctx->size >= 4) {
            uint32_t seq = htonl((uint32_t)i);
            memcpy(buf, &seq, 4);
        }

        struct timespec t0, t1;
        if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
            perror("clock_gettime");
            ctx->rtt_us[i] = -1;
            continue;
        }

        ssize_t sret = send(fd, buf, ctx->size, 0);
        if (sret < 0) {
            perror("send");
            ctx->rtt_us[i] = -1;
            continue;
        }

        ssize_t rret = recv(fd, buf, ctx->size, 0);
        if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
            perror("clock_gettime");
            ctx->rtt_us[i] = -1;
            continue;
        }

        if (rret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ctx->rtt_us[i] = -1;
                ctx->timeouts++;
            } else {
                perror("recv");
                ctx->rtt_us[i] = -1;
            }
        } else {
            ctx->rtt_us[i] = tsdiff_ns(t0, t1);
            ctx->ok++;
        }
    }

    free(buf);
    close(fd);
    return NULL;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --server IP [--threads N] [--count K] [--size BYTES]\n"
        "          [--base-port P] [--timeout-ms MS] [--outfile PATH]\n"
        "\n"
        "Defaults: threads=%d count=%d size=%d base-port=%d timeout-ms=%d outfile=%s\n",
        p, DEF_THREADS, DEF_COUNT, DEF_SIZE, DEF_BASE_PORT, DEF_TIMEOUT_MS, DEF_OUTFILE);
}

int main(int argc, char **argv) {
    static struct option opts[] = {
        {"server",     required_argument, 0, 's'},
        {"threads",    required_argument, 0, 't'},
        {"count",      required_argument, 0, 'c'},
        {"size",       required_argument, 0, 'z'},
        {"base-port",  required_argument, 0, 'p'},
        {"timeout-ms", required_argument, 0, 'm'},
        {"outfile",    required_argument, 0, 'o'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };

    char  server[64] = "";
    int   nthreads   = DEF_THREADS;
    int   count      = DEF_COUNT;
    size_t size      = DEF_SIZE;
    uint16_t base    = DEF_BASE_PORT;
    int   timeout_ms = DEF_TIMEOUT_MS;
    char  outfile[512];
    int   outfile_given = 0;                  // <— NEW
    strncpy(outfile, DEF_OUTFILE, sizeof(outfile)-1);
    outfile[sizeof(outfile)-1] = '\0';

    int c;
    while ((c = getopt_long(argc, argv, "s:t:c:z:p:m:o:h", opts, NULL)) != -1) {
        switch (c) {
            case 's': strncpy(server, optarg, sizeof(server)-1); server[sizeof(server)-1] = '\0'; break;
            case 't': nthreads = atoi(optarg); break;
            case 'c': count    = atoi(optarg); break;
            case 'z': size     = (size_t)strtoull(optarg, NULL, 10); break;
            case 'p': base     = (uint16_t)atoi(optarg); break;
            case 'm': timeout_ms = atoi(optarg); break;
            case 'o':
                strncpy(outfile, optarg, sizeof(outfile)-1);
                outfile[sizeof(outfile)-1] = '\0';
                outfile_given = 1;                    // <— NEW
                break;
            case 'h': default: usage(argv[0]); return (c=='h'?0:1);
        }
    }

    if (server[0] == '\0') { usage(argv[0]); return 1; }
    if (nthreads <= 0 || count <= 0 || size == 0) {
        fprintf(stderr, "Invalid args: threads>0, count>0, size>0 required\n");
        return 1;
    }
    if (!outfile_given) {
        // udp_result_[nthread]_[packet_size].csv
        // (size is BYTES as passed via --size)
        snprintf(outfile, sizeof(outfile), "udp_result_%d_%zu.csv", nthreads, size);
    }

    // ---------- allocate per-thread results ----------
    struct thread_ctx *ctx = (struct thread_ctx *)calloc(nthreads, sizeof(*ctx));
    pthread_t *ths = (pthread_t *)calloc(nthreads, sizeof(*ths));
    if (!ctx || !ths) { perror("calloc"); return 1; }

    for (int i = 0; i < nthreads; i++) {
        ctx[i].rtt_us = (long long *)malloc(sizeof(long long) * (size_t)count);
        if (!ctx[i].rtt_us) { perror("malloc rtts"); return 1; }
        for (int k = 0; k < count; k++) ctx[i].rtt_us[k] = -1;
    }

    // ---------- launch threads ----------
    for (int i = 0; i < nthreads; i++) {
        ctx[i].tidx       = i;
        strncpy(ctx[i].server, server, sizeof(ctx[i].server)-1);
        ctx[i].server[sizeof(ctx[i].server)-1] = '\0';
        ctx[i].dport      = (uint16_t)(base + i);
        ctx[i].count      = count;
        ctx[i].size       = size;
        ctx[i].timeout_ms = timeout_ms;

        int rc = pthread_create(&ths[i], NULL, thread_main, &ctx[i]);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create");
        }
    }

    // ---------- wait for all ----------
    for (int i = 0; i < nthreads; i++) {
        if (ths[i]) pthread_join(ths[i], NULL);
    }

    // Print averages to stdout
    print_per_connection_avg(ctx, nthreads, count);

    // ---------- single CSV after all complete ----------
    FILE *f = fopen(outfile, "w");
    if (!f) { perror("fopen outfile"); return 1; }
    fprintf(f, "pkt_index,size,RTT,port\n");
    for (int i = 0; i < nthreads; i++) {
        for (int k = 0; k < count; k++) {
            if (ctx[i].rtt_us[k] < 0) {
                // timeout sentinel stays plain -1
                fprintf(f, "%d,%zu,-1,%u\n", k, ctx[i].size, (unsigned)ctx[i].dport);
            } else {
                double us = ctx[i].rtt_us[k] / 1000.0; // ns -> µs
                fprintf(f, "%d,%zu,%.2f,%u\n", k, ctx[i].size, us, (unsigned)ctx[i].dport);
            }
        }
    }
    fclose(f);
    printf("Wrote %s\n", outfile);

    // ---------- cleanup ----------
    for (int i = 0; i < nthreads; i++) free(ctx[i].rtt_us);
    free(ths);
    free(ctx);
    return 0;
}
