#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>  
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/select.h>
#include <stdbool.h>
#include <time.h>
#include <sched.h>    // Required for cpu_set_t, CPU_ZERO, CPU_SET, sched_setaffinity
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#include "common.h"
#include "accnet_ioctl.h"
#include "iocache_ioctl.h"

#include "accnet_lib.h"
#include "iocache_lib.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#include <bits/cpu-set.h>

#define MODE_POLLING "poll"     // Polling Client
#define MODE_BLOCKING "block"   // Blocking Client
#define MODE_SERVER "server"    // Blocking Server
#define MODE_LOOP "loop"
#define MODE_SINK  "sink"

struct timespec timespec_diff(struct timespec *start, struct timespec *end);
struct timespec timespec_from_tick(uint64_t ns);
uint64_t test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
                                        uint8_t payload[], uint32_t payload_size, bool debug);
uint64_t test_udp_latency_poll(struct accnet_info *accnet, struct iocache_info *iocache, 
                                        uint8_t payload[], uint32_t payload_size, bool debug);
void test_udp_server_block(struct accnet_info *accnet, struct iocache_info *iocache, bool debug);
static uint64_t test_loopback_throughput_local(struct accnet_info *accnet, struct iocache_info *iocache,
                                         uint32_t payload_size, size_t target_bytes, bool debug);
void test_udp_server_throughput(struct accnet_info *accnet, struct iocache_info *iocache, 
                                uint32_t payload_size, uint32_t ntest, bool debug);

static inline bool is_power_of_two_u32(uint32_t x) {
    return x && ((x & (x - 1)) == 0);
}

uint64_t time_ns[8] = {0};
uint64_t network_ticks = 0;

/* 1) Global flag set by Ctrl-C (SIGINT) */
static volatile sig_atomic_t g_got_sigint = 0;

/* 2) Minimal, async-signal-safe handler */
static void on_sigint(int signo) {
    (void)signo;
    g_got_sigint = 1;           // just set a flag; do nothing else here
}

/* 3) Install handler (SIGINT + optional SIGTERM) */
static void install_sig_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            // NO SA_RESTART: interrupt blocking syscalls
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);  // optional: treat kill -TERM like Ctrl-C
}

static inline uint64_t rdcycle(void)
{
    uint64_t x;
    __asm__ volatile ("rdcycle %0" : "=r"(x));
    return x;
}

static void pin_to_cpu(int cpu) {

    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (cpu < 0 || cpu >= ncpu) {
        fprintf(stderr, "cpu %d out of range [0..%ld)\n", cpu, ncpu);
        exit(1);
    }

    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity"); /* continue anyway */
    }

    // struct sched_param sp = { .sched_priority = 10 }; // SCHED_FIFO 1..99
    // sched_setscheduler(0, SCHED_FIFO, &sp);
    // mlockall(MCL_CURRENT|MCL_FUTURE);
}

// Compute free bytes in a byte-addressed circular ring.
static inline uint32_t ring_free_bytes(uint32_t head, uint32_t tail, uint32_t size)
{
    // "one byte empty" convention to distinguish full vs empty
    if (tail >= head) return (size - (tail - head) - 1);
    return (head - tail - 1);
}

// Copy 'len' bytes into a wrapping ring buffer at 'tail'
static inline void ring_memcpy_wrap(uint8_t *base, uint32_t size, uint32_t tail, const uint8_t *src, uint32_t len)
{
    /* Let's skip copying... */
    // uint32_t first = len;
    // if (tail + len > size) first = size - tail;
    // memcpy(base + tail, src, first);
    // if (first < len) {
    //     memcpy(base, src + first, len - first);
    // }
}

// Fill at most 1/4 of the TX ring this call; return bytes actually queued
static uint32_t try_fill_tx(struct accnet_info *accnet, struct iocache_info *iocache,
                            const uint8_t *payload, uint32_t chunk_bytes,
                            uint64_t need_bytes, bool debug)
{
    int row = iocache->row;

    // Snapshot ring pointers/sizes
    uint32_t tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    uint32_t tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    uint32_t tx_size = reg_read32(iocache->regs,         IOCACHE_REG_TX_RING_SIZE(row));
    mmio_rmb();

    // Free space with "one empty byte" convention
    uint32_t freeb = ring_free_bytes(tx_head, tx_tail, tx_size);

    // Cap to one quarter of the ring per call, and also to what's needed
    uint32_t quarter = tx_size / 2u;
    uint32_t quota   = quarter;
    if (quota > freeb)             quota = freeb;
    if ((uint64_t)quota > need_bytes) quota = (uint32_t)need_bytes;

    // Only send whole chunks
    quota -= (quota % chunk_bytes);
    if (quota == 0) return 0;

    // Copy 'quota' bytes in chunk_bytes steps, then publish the new tail once
    uint32_t sent = 0;
    while (sent < quota) {
        ring_memcpy_wrap((uint8_t*)iocache->udp_tx_buffer, tx_size, tx_tail,
                         payload, chunk_bytes);
        tx_tail = (tx_tail + chunk_bytes) % tx_size;
        sent   += chunk_bytes;
    }

    mmio_wmb();
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), tx_tail);

    if (debug) {
        printf("try_fill_tx: quarter=%u freeb=%u quota=%u sent=%u new_tail=%u\n",
               quarter, freeb, quota, sent, tx_tail);
    }
    return sent;
}


// Drain RX; returns bytes drained this call
static uint32_t drain_rx(struct accnet_info *accnet, struct iocache_info *iocache)
{
    int row = iocache->row;

    uint32_t rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    uint32_t rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    uint32_t rx_size = reg_read32(iocache->regs,         IOCACHE_REG_RX_RING_SIZE(row));
    mmio_rmb();

    uint32_t avail = (rx_tail >= rx_head) ? (rx_tail - rx_head) : (rx_size - (rx_head - rx_tail));
    if (avail) {
        // Consume everything we see
        mmio_wmb();
        reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);
    }
    return avail;
}

// Loopback throughput test (returns elapsed ticks)
static uint64_t test_loopback_throughput(struct accnet_info *accnet, struct iocache_info *iocache,
                                         uint32_t payload_size, size_t target_bytes, bool debug)
{
    // Prepare a deterministic payload
    uint8_t *payload = alloca(payload_size);
    for (uint32_t i = 0; i < payload_size; i++) payload[i] = (uint8_t)(i & 0xff);

    size_t total_tx = 0, total_rx = 0;

    // Start time just before first TX enqueue
    uint64_t t_start = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();

    while (!g_got_sigint && total_rx < target_bytes) {
        // 1) Try to fill TX as much as possible this iteration
        if (total_tx < target_bytes) {
            uint64_t need = target_bytes - total_tx;
            uint32_t just_tx = try_fill_tx(accnet, iocache, payload, payload_size, need, debug);
            total_tx += just_tx;
        }

        // 2) Drain whatever RX arrived; advance RX_HEAD
        uint32_t just_rx = drain_rx(accnet, iocache);
        total_rx += just_rx;

        // If neither progressed, do a very small pause (or just continue tight)
        if (just_rx == 0 && (total_tx >= target_bytes)) {
            iocache_wait_on_rx(iocache);
        }
    }

    uint64_t t_end = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();

    if (debug) {
        printf("Loopback summary: TX=%zu, RX=%zu, ticks=%" PRIu64 "\n", total_tx, total_rx, (t_end - t_start));
    }

    // Print stats
    uint64_t ticks = (t_end - t_start);
    double seconds = ticks * (US_PER_TICK / 1e6);   // US_PER_TICK from your common.h
    double mbps = (total_rx * 8.0) / (1e6 * seconds);
    double gbps = mbps / 1000.0;

    printf("Loopback throughput: TX=%zu B, RX=%zu B, time=%.3f ms, rate=%.1f Mb/s (%.2f Gb/s)\n",
           total_tx, total_rx, seconds * 1e3, mbps, gbps);

    return ticks;
}

struct tx_worker_args {
    struct accnet_info   *accnet;
    struct iocache_info  *iocache;
    const uint8_t        *payload;
    uint32_t              payload_size;
    uint32_t              ntest;
    bool                  debug;
    int                   pin_cpu;      // pin worker here (we'll use 3)
};

#include <sys/resource.h>

static void *tx_worker_fn(void *arg) {
    struct tx_worker_args *a = (struct tx_worker_args *)arg;

    struct sched_param sp0 = { .sched_priority = 0 };
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp0) == -1) {
        perror("pthread_setschedparam TX->SCHED_OTHER");
    }

    uint32_t tx_head, tx_tail, tx_size;
    uint32_t rx_head, rx_tail, rx_size;
    int row = a->iocache->row;

    int counter = 0;

    /* Preparing to send the payload */
    tx_head = reg_read32(a->accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(a->accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(a->iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));

    do {
        tx_head = reg_read32(a->accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
        mmio_rmb();
        uint32_t avail = (tx_tail >= tx_head) ? (tx_size - (tx_tail - tx_head) - 1) : (tx_head - tx_tail - 1);

        // printf("in send thread...\n");
        usleep(30);

        if (a->payload_size <= avail) {
            // printf("sending...\n");
            // No need for copying
            uint32_t new_tx_tail = (tx_tail + a->payload_size) % a->iocache->udp_tx_size;
            reg_write32(a->accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), new_tx_tail);
            mmio_wmb();
            tx_tail = new_tx_tail;
        }

    } while (counter++ < a->ntest && !g_got_sigint);

    printf("sending done!\n");
    return NULL;
}


int main(int argc, char **argv) {
    int cpu = 0;
    char *accnet_filename = "/dev/accnet-misc";
    char *iocache_filename = "/dev/iocache-misc";
    int n_tests = 64;
    bool debug = false;
    bool skip_first = false;
    bool skip_file = false;
    bool reset = false;
    bool print_all = false;
    uint32_t payload_size = 1*1024;
    char *src_ip = "10.0.0.2";
    char *src_mac = "0c:42:a1:a8:2d:e6";
    uint16_t src_port = 1111;
    char *dst_ip = "10.0.0.1";
    char *dst_mac = "00:0a:35:06:4d:e2";
    uint16_t dst_port = 1234;
    uint16_t client_id = 0;
    char *mode = MODE_POLLING;
    int ring = 0;
    size_t target_bytes = 1 * 1024 * 1024; // 1 MiB

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--cpu A]"
                "[--ntest N] [--mode {" MODE_POLLING "|" MODE_BLOCKING "|" MODE_SERVER "}]"
                "[--payload-size BYTES] [--ring R]"
                "[--src-ip ADDR] [--src-port PORT] "
                "[--dst-ip ADDR] [--dst-port PORT] "
                "[--client-id ID]"
                "[--reset] [--skip-outfile]"
                "[--debug] [--print-all] [--skip-first]\n", argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            long long v = atoll(argv[++i]);
            if (v <= 0) { fprintf(stderr, "Invalid --bytes value\n"); return -1; }
            target_bytes = (size_t)v;
        }
        else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu = atoi(argv[++i]);
            // printf("Parsed --cpu = %d\n", cpu);
        }
        else if (strcmp(argv[i], "--client-id") == 0 && i + 1 < argc) {
            client_id = atoi(argv[++i]);
            // printf("Parsed --client-id = %d\n", client_id);
        }
        else if (strcmp(argv[i], "--ring") == 0 && i + 1 < argc) {
            ring = atoi(argv[++i]);
            // printf("Parsed --ring = %d\n", ring);
        }
        else if (strcmp(argv[i], "--ntest") == 0 && i + 1 < argc) {
            n_tests = atoi(argv[++i]);
            // printf("Parsed --ntest = %d\n", n_tests);
        }
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            if (strcmp(mode, MODE_POLLING) == 0 ||
                strcmp(mode, MODE_BLOCKING) == 0 ||
                strcmp(mode, MODE_SERVER)  == 0 ||
                strcmp(mode, MODE_LOOP)    == 0 ||
                strcmp(mode, MODE_SINK)   == 0) {
                // ok
            } else {
                fprintf(stderr, "Invalid mode (options: %s, %s, %s, %s, %s)\n",
                    MODE_POLLING, MODE_BLOCKING, MODE_SERVER, MODE_LOOP, MODE_SINK);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--payload-size") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val <= 0 || val % 64 != 0) {
                fprintf(stderr, "Invalid payload size (val <= 0 || val %% 64 != 0)\n");
                return -1;
            }
            payload_size = (uint32_t) val;
            // printf("Parsed --payload-size = %d\n", payload_size);
        }
        else if (strcmp(argv[i], "--src-mac") == 0 && i + 1 < argc) {
            src_mac = argv[++i];
            // printf("Parsed --src-mac = %s\n", src_mac);
        }
        else if (strcmp(argv[i], "--src-ip") == 0 && i + 1 < argc) {
            src_ip = argv[++i];
            // printf("Parsed --src-ip = %s\n", src_ip);
        }
        else if (strcmp(argv[i], "--src-port") == 0 && i + 1 < argc) {
            src_port = (uint16_t) atoi(argv[++i]);
            // printf("Parsed --src-port = %u\n", src_port);
        }
        else if (strcmp(argv[i], "--dst-mac") == 0 && i + 1 < argc) {
            dst_mac = argv[++i];
            // printf("Parsed --dst-mac = %s\n", dst_mac);
        }
        else if (strcmp(argv[i], "--dst-ip") == 0 && i + 1 < argc) {
            dst_ip = argv[++i];
            // printf("Parsed --dst-ip = %s\n", dst_ip);
        }
        else if (strcmp(argv[i], "--dst-port") == 0 && i + 1 < argc) {
            dst_port = (uint16_t) atoi(argv[++i]);
            // printf("Parsed --dst-port = %u\n", dst_port);
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
            // printf("Parsed --debug\n");
        }
        else if (strcmp(argv[i], "--skip-first") == 0) {
            skip_first = true;
            // printf("Parsed --skip-first\n");
        }
        else if (strcmp(argv[i], "--skip-file") == 0) {
            skip_file = true;
            // printf("Parsed --skip-file\n");
        }
        else if (strcmp(argv[i], "--reset") == 0) {
            reset = true;
            // printf("Parsed --reset\n");
        }
        else if (strcmp(argv[i], "--print-all") == 0) {
            print_all = true;
            // printf("Parsed --print-all\n");
        }
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
    fflush(stdout);

    // if (payload_size > 1024 && (!is_power_of_two_u32(payload_size) || !is_power_of_two_u32((uint32_t) n_tests))) {
    //     printf("bad inputs\n");
    //     return 1;
    // }

    pin_to_cpu(cpu);
    install_sig_handlers();

    bool is_polling  = strcmp(mode, MODE_POLLING) == 0;
    bool is_blocking = strcmp(mode, MODE_BLOCKING) == 0;
    bool is_server   = strcmp(mode, MODE_SERVER)  == 0;
    bool is_loop     = strcmp(mode, MODE_LOOP)    == 0;
    bool is_async    = strcmp(mode, MODE_SINK)   == 0;

    struct accnet_info *accnet      = malloc(sizeof(struct accnet_info));
    struct iocache_info *iocache    = calloc(1, sizeof(*iocache));
    long long *rtts            = calloc(n_tests, sizeof(long long));
    long long *network_latency = calloc(n_tests, sizeof(long long)); // if you keep it
    if (!rtts || !network_latency) { perror("calloc"); return 1; }

    if (iocache_open(iocache_filename, iocache, ring) < 0) {
        fprintf(stderr, "iocache_open failed\n"); 
        return 1;
    }

    // printf("row is %d\n", iocache->row);

    if (accnet_open(accnet_filename, accnet, iocache, true) < 0) {
        fprintf(stderr, "accnet_open failed\n"); 
        return 1;
    }

    if (reset) {
        for (int i = 0; i < IOCACHE_CACHE_ENTRY_COUNT; i++) {
            reg_write32(iocache->regs, IOCACHE_REG_PROC_PTR(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_ENABLED(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_RX_SUSPENDED(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_RX_RING_ADDR(i), 0);
            reg_write32(iocache->regs, IOCACHE_REG_TX_RING_ADDR(i), 0);
        }
        accnet_close(accnet);
        iocache_close(iocache);
        return 0;
    }

    struct connection_info *conn = malloc(sizeof(struct connection_info));
    if (conn_from_strings_mac(conn, 0x11, src_mac, src_ip, src_port, dst_mac, dst_ip, dst_port) != 0) {
        fprintf(stderr, "conn_from_strings failed\n"); return 1;
    }

    accnet_setup_connection(accnet, conn);
    iocache_setup_connection(iocache, conn);

    // if (is_blocking)
    //     iocache_start_scheduler(iocache);

    /* Init rings */
    // accnet_start_ring(accnet);

    if (is_loop) {
        test_loopback_throughput_local(accnet, iocache, payload_size, target_bytes, debug);
    }
    else if (is_server) {
        test_udp_server_block(accnet, iocache, debug);
    }
    else if (is_async) {
        test_udp_server_throughput(accnet, iocache, payload_size, n_tests, debug);
    }
    else {
        // This is client mode

        /* Initializing payload */
        uint8_t payload[payload_size];
        for (uint32_t i = 0; i < payload_size; i++) {
            payload[i] = i & 0xff;
        }

        long long sum_ticks = 0, min_ticks = 0, max_ticks = 0;
        uint64_t sum_network_latency_tick = 0;
    
        /* Run Test */
        int received_ok = 0;
        int received_total = 0;
        for (int i = 0; i < n_tests && !g_got_sigint; i++) {
            uint64_t diff = 0;
    
            if (strcmp(mode, MODE_POLLING) == 0) {
                diff = test_udp_latency_poll(accnet, iocache, payload, payload_size, debug); 
            }
            else if (strcmp(mode, MODE_BLOCKING) == 0) {
                diff = test_udp_latency_block(accnet, iocache, payload, payload_size, debug);
            }
            if (diff == 0) {
                continue;
            }
    
            ++received_total;
    
            /* skip first packet entirely */
            if (i == 0 && skip_first)
                continue;
    
            uint64_t netdelay_tick   = accnet_get_outside_ticks(accnet);
    
            if (i == 0 || min_ticks == 0) {
                min_ticks = max_ticks = diff;
            } else {
                if (diff < min_ticks) min_ticks = diff;
                if (diff > max_ticks) max_ticks = diff;
            }
            sum_ticks += diff;
            rtts[i] = diff;
    
            sum_network_latency_tick += netdelay_tick;
            network_latency[i] = netdelay_tick;
    
            ++received_ok;
            // printf("iter=%d rtt=%.3f us\n", i, diff / 1e3);
        }
        // iocache_print_proc_util(iocache);
    
        double avg_us           = (sum_ticks                   / (double)received_ok) * US_PER_TICK;
        double avg_network_us   = (sum_network_latency_tick / (double)received_ok) * US_PER_TICK;
    
        printf("\nResults (%s): recv=%d/%d  min=%.2f us , avg=%.2f us , max=%.2f us , network=%.2f us\n\n",
                    mode, received_total, n_tests, 
                    min_ticks * US_PER_TICK, avg_us, max_ticks * US_PER_TICK, avg_network_us);

        // printf("Time breakdown average:\nEntry-Before: %.3f us\nPLIC-Entry: %.3f us\nIRQ-PLIC: %.3f us\n"
        //     "Syscall-IRQ: %.3f us\nAfter-Syscall: %.3f us\n",
        //         time_ns[0]/(double)received_ok/1e3, 
        //         time_ns[1]/(double)received_ok/1e3, 
        //         time_ns[2]/(double)received_ok/1e3, 
        //         time_ns[3]/(double)received_ok/1e3,
        //         time_ns[4]/(double)received_ok/1e3
        //     );
    
        if (print_all) {
            for (int i = 0; i < n_tests; i++) {
                printf("iter=%d rtt=%.3f us\n", i, rtts[i] * US_PER_TICK / 1e3);
            }
        }

        if (!skip_file) {
            // ---- Write results file: out-accio-[payloadsize]-[srcPort].csv ----
            char out_path[128];
            snprintf(out_path, sizeof(out_path), "out-accio-%u-%u.csv", payload_size, src_port);
    
            FILE *fout = fopen(out_path, "w");
            if (!fout) {
                perror("fopen out-accio-[payload]-[srcPort].csv");
            } else {
                // Header
                fprintf(fout, "pkt_index,size(B),RTT(us),network_time(us)\n");
    
                // Rows: only write entries that were actually measured (rtts[i] > 0)
                for (int i = 0; i < n_tests; i++) {
                    if (rtts[i] > 0) {
                        double rtt_us = rtts[i] * US_PER_TICK;              
                        double net_us = network_latency[i] * US_PER_TICK;     
                        fprintf(fout, "%d,%u,%.3f,%.3f\n", i, payload_size, rtt_us, net_us);
                    }
                }
                fclose(fout);
                printf("Wrote results to %s\n", out_path);
            }
        }
    }


    // if (is_blocking)
    //     iocache_stop_scheduler(iocache);

    iocache_clear_connection(iocache);

    /* Close Accnet */
    accnet_close(accnet);

    iocache_close(iocache);
    exit(0);
    return 0;
}

void test_udp_server_throughput(struct accnet_info *accnet, struct iocache_info *iocache, 
                                uint32_t payload_size, uint32_t ntest, bool debug) {
    // Prepare a deterministic payload once
    uint8_t *payload = aligned_alloc(64, payload_size);
    if (!payload) { perror("aligned_alloc"); return 1; }
    for (uint32_t i = 0; i < payload_size; i++) payload[i] = (uint8_t)(i & 0xff);

    pthread_t tx_thr;
    pthread_attr_t attr;
    cpu_set_t cpus;

    pthread_attr_init(&attr);

    // Launch TX worker pinned to CPU 3
    struct tx_worker_args args = {
        .accnet       = accnet,
        .iocache      = iocache,
        .payload      = payload,
        .payload_size = payload_size,
        .ntest        = ntest,
        .debug        = debug,
        .pin_cpu      = 1
    };

    // RX draining on main thread; record first/last RX timestamps & bytes
    size_t   total_rx   = 0;
    uint64_t first_tick = 0;
    uint64_t last_tick  = 0;
    volatile uint64_t now;
    int row = iocache->row;

    uint32_t rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    uint32_t rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    uint32_t rx_size = reg_read32(iocache->regs,         IOCACHE_REG_RX_RING_SIZE(row));
    mmio_rmb();

    // if (pthread_create(&tx_thr, &attr, tx_worker_fn, &args) != 0) {
    //     perror("pthread_create");
    //     pthread_attr_destroy(&attr);
    //     free(payload);
    //     return 1;
    // }

    pthread_attr_destroy(&attr);

    while (!g_got_sigint) {
        int ret = iocache_wait_on_rx(iocache);

        if (ret != 0) {
            // timeout path (no-op)
            continue;
        }

        // printf("received packet...\n");

        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
        mmio_rmb();

        uint32_t got = (rx_tail >= rx_head) ? (rx_tail - rx_head) : (rx_size - (rx_head - rx_tail));
        if (got) {
            // printf("received %u...\n", got);

            // Consume everything we see
            mmio_wmb();
            reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);
            rx_head = rx_tail; // update software view
            
            now = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
            mmio_rmb();
            if (first_tick == 0) {
                first_tick = now;
            }
            last_tick = now;
            total_rx += got;
        } 
    }

    // // Stop TX worker and join
    // pthread_join(tx_thr, NULL);

    // Compute throughput between first and last RX timestamps
    if (first_tick == 0 || last_tick <= first_tick) {
        printf("No RX observed (or invalid timestamps). Nothing to report.\n");
    } else {
        uint64_t ticks = last_tick - first_tick;
        double seconds = ticks * (US_PER_TICK / 1e6);   // device tick -> seconds
        double mbps    = (total_rx * 8.0) / (1e6 * seconds);
        double gbps    = mbps / 1000.0;
        printf("[async] RX bytes=%zu  window=%.3f ms  rate=%.1f Mb/s (%.2f Gb/s)\n",
            total_rx, seconds*1e3, mbps, gbps);
    }

    free(payload);
}
void test_udp_server_block(struct accnet_info *accnet, struct iocache_info *iocache, bool debug) {
    int ret;
    int row = iocache->row;
    volatile uint64_t before, after;
    uint32_t nPackets = 0;
    volatile total_ticks = 0;
    volatile total_ticks_2 = 0;
    
    uint32_t size;
    uint32_t new_tx_tail;
    uint32_t tx_head, tx_tail, tx_size;
    uint32_t rx_head, rx_tail, rx_size;
    
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));

    /* Preparing to receive the payload */
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    rx_size = reg_read32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row));

    if (tx_head != tx_tail) {
        printf("TX is weird!\n");
        return;
    }

    if (rx_head != rx_tail) {
        printf("RX is weird!\n");
        return;
    }

    do {
        ret = iocache_wait_on_rx(iocache);

        if (ret == 0) {
            // Means there is a packet
            rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
            mmio_rmb();

            if (rx_tail == rx_head) {
                size = 0;
                printf("Size is weird!\n");
                continue;
            } else if (rx_tail > rx_head) {
                size = rx_tail - rx_head;
            } else {
                size = rx_size - (rx_head - rx_tail);
            }

            // Copy from RX ring -> TX ring
            memcpy((char *)iocache->udp_tx_buffer + tx_tail, (char *)iocache->udp_rx_buffer + rx_head, size);

            // We are done consuming RX bytes up to rx_tail: publish new RX_HEAD
            mmio_wmb();
            reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);

            // Advance TX tail and notify HW
            new_tx_tail = (tx_tail + size) % iocache->udp_tx_size;

            mmio_wmb();
            reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), new_tx_tail);
            after = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);

            // update our software view
            rx_head = rx_tail;
            tx_tail = new_tx_tail;
            nPackets++;
            volatile rx_timestamp = reg_read64(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_LAST_TIMESTAMP(row));
            volatile tx_timestamp = reg_read64(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_LAST_TIMESTAMP(row));
            total_ticks += tx_timestamp - rx_timestamp;
            total_ticks_2 += after - rx_timestamp;
        }
        else {
            // timeout path (no-op)
        }

    } while (!g_got_sigint);

    printf("\nResults (%s): recv=%d, avg1=%.2f us, avg2=%.2f us\n\n",
                    MODE_SERVER, nPackets, 
                    (total_ticks / (double)nPackets) * US_PER_TICK,
                    (total_ticks_2 / (double)nPackets) * US_PER_TICK);
}

static uint64_t test_loopback_throughput_local(struct accnet_info *accnet, struct iocache_info *iocache,
                                         uint32_t payload_size, size_t target_bytes, bool debug) {
    int row = iocache->row;
    uint64_t before, after;
    int ret;

    uint32_t tx_head, tx_tail, tx_size;
    uint32_t rx_head, rx_tail, rx_size;

    /* Preparing to send the payload */
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));

    /* Preparing to receive the payload */
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    rx_size = reg_read32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row));
    mmio_rmb();
                                
    size_t total_tx = 0, total_rx = 0;

    before = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    while (!g_got_sigint && total_rx < target_bytes) {
        uint32_t new_tx_tail = (tx_tail + payload_size) % iocache->udp_tx_size;
        
        mmio_rmb();
        reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), new_tx_tail);
        mmio_wmb();
        
        tx_tail = new_tx_tail;
        do {
            tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
        } while (tx_head != tx_tail);

        /* Wait for RX */
        ret = iocache_wait_on_rx(iocache);
        mmio_rmb();

        /* Updating RX HEAD */
        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
        int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
        reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);
        rx_head = rx_tail;
        total_rx += size;
    }
    after = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);

    // Print stats
    uint64_t ticks = (after - before);
    double seconds = ticks * (US_PER_TICK / 1e6);   // US_PER_TICK from your common.h
    double mbps = (total_rx * 8.0) / (1e6 * seconds);
    double gbps = mbps / 1000.0;

    printf("Loopback throughput: TX=%zu B, RX=%zu B, time=%.3f ms, rate=%.1f Mb/s (%.2f Gb/s)\n",
           total_tx, total_rx, seconds * 1e3, mbps, gbps);

}
uint64_t test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
uint8_t payload[], uint32_t payload_size, bool debug) 
{
    int row = iocache->row;
    uint64_t before, after;
    uint64_t a1, a2;
    __u64 last_ktimes[5] = {0};
    int ret;

    /* Preparing to send the payload */
    uint32_t tx_head, tx_tail, tx_size;
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));

    /* Preparing to receive the payload */
    uint32_t rx_head, rx_tail, rx_size;
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    rx_size = reg_read32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row));
    mmio_rmb();

    /* Triggering TX and starting time */
    memcpy((char *)iocache->udp_tx_buffer + tx_tail, payload, payload_size);
    uint32_t new_tx_tail = (tx_tail + payload_size) % iocache->udp_tx_size;


    // clock_gettime(CLOCK_MONOTONIC, &before);
    before = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), new_tx_tail);
    mmio_wmb();

    /* Waiting for RX */
    ret = iocache_wait_on_rx(iocache);
    after = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();

    /* Updating RX HEAD */
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);

    if (ret == -1) return 0;

    // /* IRQ timestamp */
    // if (iocache_get_last_ktimes(iocache, last_ktimes) == 0) {
    //     struct timespec irq_ts, plic_ts, entry_ts, awake_ts, diff1, diff2, diff3, diff4, diff5;
    //     entry_ts.tv_sec  = last_ktimes[0] / 1000000000ULL;
    //     entry_ts.tv_nsec = last_ktimes[0] % 1000000000ULL;
    //     plic_ts.tv_sec  = last_ktimes[1] / 1000000000ULL;
    //     plic_ts.tv_nsec = last_ktimes[1] % 1000000000ULL;
    //     irq_ts.tv_sec  = last_ktimes[2] / 1000000000ULL;
    //     irq_ts.tv_nsec = last_ktimes[2] % 1000000000ULL;
    //     awake_ts.tv_sec  = last_ktimes[3] / 1000000000ULL;
    //     awake_ts.tv_nsec = last_ktimes[3] % 1000000000ULL;

    //     diff1 = timespec_diff(&before, &entry_ts);
    //     diff2 = timespec_diff(&entry_ts, &plic_ts);
    //     diff3 = timespec_diff(&plic_ts, &irq_ts);
    //     diff4 = timespec_diff(&irq_ts, &awake_ts);
    //     diff5 = timespec_diff(&awake_ts, &after);

    //     time_ns[0] += diff1.tv_sec * 1e9 + diff1.tv_nsec;
    //     time_ns[1] += diff2.tv_sec * 1e9 + diff2.tv_nsec;
    //     time_ns[2] += diff3.tv_sec * 1e9 + diff3.tv_nsec;
    //     time_ns[3] += diff4.tv_sec * 1e9 + diff4.tv_nsec;
    //     time_ns[4] += diff5.tv_sec * 1e9 + diff5.tv_nsec;
    // } else {
    //     printf("Either 'iocache_get_last_irq_ns' or 'iocache_get_last_ktimes' failed\n");
    // }

    return after - before;
}

uint64_t test_udp_latency_poll(struct accnet_info *accnet, struct iocache_info *iocache, 
uint8_t payload[], uint32_t payload_size, bool debug)
{
    uint64_t before, after;
    int row = iocache->row;

    int counter = 0;

    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;

    // Read buffer head/tail
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    rx_size = reg_read32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row));


    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));
    mmio_rmb();

    if (debug) {
        printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);
        printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);
    }

    
    if (debug) printf("Copying mem...\n");
    memcpy((char *)iocache->udp_tx_buffer + tx_tail, payload, payload_size);
    
    uint32_t val = (tx_tail + payload_size) % iocache->udp_tx_size;
    if (debug) printf("Begin Test... (new_tail=%u) \n", val);
    

    before = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();

    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), val);
    do {
        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
        mmio_rmb();
        ++counter;
        if (debug) {
            printf("Waiting for RX (new rx_tail=%u)...\n", rx_tail);
        }
    } while (rx_tail != (rx_head + payload_size) % iocache->udp_rx_size && !g_got_sigint);
    
    after = reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
    mmio_rmb();


    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row));
    rx_size = reg_read32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row));
    
    // Read buffer head/tail
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row));
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row));
    tx_size = reg_read32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row));
    mmio_rmb();

    if (debug) {
        printf("RX waited %d times\n", counter);

        printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);
        printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);

        printf("Original Payload:\n");
        for (uint32_t i = 0; i < payload_size; i++) {
            printf("%02x", payload[i]);
        }
        printf("\n");
        printf("RX Payload:\n");
        for (uint32_t i = rx_head; i < rx_tail; i++) {
            printf("%02x", ((uint8_t*)iocache->udp_rx_buffer)[i]);
        }
        printf("\n");
    }

    // Updating RX HEAD
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);

    return after - before;
}

struct timespec timespec_diff(struct timespec *start, struct timespec *end) {
    struct timespec temp;
    temp.tv_sec  = end->tv_sec  - start->tv_sec;
    temp.tv_nsec = end->tv_nsec - start->tv_nsec;

    if (temp.tv_nsec < 0) {
        temp.tv_sec -= 1;
        temp.tv_nsec += 1000000000L;
    }
    return temp;
}

struct timespec timespec_from_tick(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec  = ns / 1000000000ULL;         // whole seconds
    ts.tv_nsec = ns % 1000000000ULL;         // leftover nanoseconds
    return ts;
}
