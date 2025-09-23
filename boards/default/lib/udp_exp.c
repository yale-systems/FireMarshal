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

#include "common.h"
#include "accnet_ioctl.h"
#include "iocache_ioctl.h"

#include "accnet_lib.h"
#include "iocache_lib.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#include <bits/cpu-set.h>

#define MODE_POLLING "poll"
#define MODE_BLOCKING "block"

struct timespec timespec_diff(struct timespec *start, struct timespec *end);
struct timespec test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
                                        uint8_t payload[], uint32_t payload_size, bool debug);
struct timespec test_udp_latency_poll(struct accnet_info *accnet, struct iocache_info *iocache, 
                                        uint8_t payload[], uint32_t payload_size, bool debug);

uint64_t time_ns[8] = {0};

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

    struct sched_param sp = { .sched_priority = 10 }; // SCHED_FIFO 1..99
    sched_setscheduler(0, SCHED_FIFO, &sp);
    mlockall(MCL_CURRENT|MCL_FUTURE);
}

int main(int argc, char **argv) {
    int cpu = 0;
    char *accnet_filename = "/dev/accnet-misc";
    char *iocache_filename = "/dev/iocache-misc";
    int n_tests = 64;
    bool debug = false;
    bool skip_first = false;
    bool reset = false;
    bool print_all = false;
    uint32_t payload_size = 1*1024;
    char *src_ip = "10.0.0.2";
    char *src_mac = "0c:42:a1:a8:2d:e6";
    uint16_t src_port = 1111;
    char *dst_ip = "10.0.0.1";
    char *dst_mac = "00:0a:35:06:4d:e2";
    uint16_t dst_port = 1234;
    char *mode = MODE_POLLING;
    int ring = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--cpu A]"
                "[--ntest N] [--mode {" MODE_POLLING "|" MODE_BLOCKING "}]"
                "[--payload-size BYTES] [--ring R]"
                "[--src-ip ADDR] [--src-port PORT] "
                "[--dst-ip ADDR] [--dst-port PORT] "
                "[--reset] "
                "[--debug] [--print-all] [--skip-first]\n", argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu = atoi(argv[++i]);
            // printf("Parsed --cpu = %d\n", cpu);
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
            if (strcmp(mode, MODE_POLLING) == 0 || strcmp(mode, MODE_BLOCKING) == 0) {
                // printf("Parsed --mode = %s\n", mode);
            }
            else {
                fprintf(stderr, "Invalid mode (options: " MODE_POLLING ", " MODE_BLOCKING ")\n");
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

    pin_to_cpu(cpu);

    bool is_polling     = strcmp(mode, MODE_POLLING) == 0;
    bool is_blocking    = strcmp(mode, MODE_BLOCKING) == 0;

    struct accnet_info *accnet      = malloc(sizeof(struct accnet_info));
    struct iocache_info *iocache    = calloc(1, sizeof(*iocache));
    long long *rtts                 = malloc(sizeof(long long) * n_tests);
    long long *network_latency      = malloc(sizeof(long long) * n_tests);

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

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    /* Init rings */
    // accnet_start_ring(accnet);

    long long sum_ns = 0, min_ns = 0, max_ns = 0;
    uint64_t sum_network_latency_tick = 0;

    /* Run Test */
    int received_ok = 0;
    int received_total = 0;
    for (int i = 0; i < n_tests; i++) {
        struct timespec diff = {0, 0};

        if (strcmp(mode, MODE_POLLING) == 0) {
            diff = test_udp_latency_poll(accnet, iocache, payload, payload_size, debug); 
        }
        else if (strcmp(mode, MODE_BLOCKING) == 0) {
            diff = test_udp_latency_block(accnet, iocache, payload, payload_size, debug);
        }
        if (diff.tv_sec == 0 && diff.tv_nsec == 0) {
            continue;
        }

        ++received_total;

        /* skip first packet entirely */
        if (i == 0 && skip_first)
            continue;

        long long rtt_ns        = diff.tv_sec * 1e9 + diff.tv_nsec;
        uint64_t netdelay_tick   = accnet_get_outside_ticks(accnet);

        if (i == 0 || min_ns == 0) {
            min_ns = max_ns = rtt_ns;
        } else {
            if (rtt_ns < min_ns) min_ns = rtt_ns;
            if (rtt_ns > max_ns) max_ns = rtt_ns;
        }
        sum_ns += rtt_ns;
        rtts[i] = rtt_ns;

        sum_network_latency_tick += netdelay_tick;

        ++received_ok;
        // printf("iter=%d rtt=%.3f us\n", i, rtt_ns / 1e3);
    }
    // iocache_print_proc_util(iocache);

    double avg_us           = (sum_ns                   / (double)received_ok) / 1e3;
    double avg_network_us   = (sum_network_latency_tick / (double)received_ok) * US_PER_TICK;

    printf("\nResults (%s): recv=%d/%d  min=%.2f us , avg=%.2f us , max=%.2f us , network=%.2f us\n\n",
                mode, received_total, n_tests, min_ns/1e3, avg_us, max_ns/1e3, avg_network_us);
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
            printf("iter=%d rtt=%.3f us\n", i, rtts[i] / 1e3);
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

struct timespec test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
uint8_t payload[], uint32_t payload_size, bool debug) 
{
    int row = iocache->row;
    struct timespec before, after;
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

    clock_gettime(CLOCK_MONOTONIC, &before);
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), new_tx_tail);
    mmio_wmb();

    /* Waiting for RX */
    ret = iocache_wait_on_rx(iocache);
    clock_gettime(CLOCK_MONOTONIC, &after);

    /* Updating RX HEAD */
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
    int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), rx_tail);

    if (ret == -1) return (struct timespec){0, 0};

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

    return timespec_diff(&before, &after);
}

struct timespec test_udp_latency_poll(struct accnet_info *accnet, struct iocache_info *iocache, 
uint8_t payload[], uint32_t payload_size, bool debug)
{
    struct timespec before, after;
    int row = iocache->row;

    // printf("poll, row is %d\n", row);
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
    
    clock_gettime(CLOCK_MONOTONIC, &before);
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), val);
    do {
        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row));
        mmio_rmb();
        ++counter;
        if (debug) {
            printf("Waiting for RX (new rx_tail=%u)...\n", rx_tail);
        }
    } while (rx_tail != (rx_head + payload_size) % iocache->udp_rx_size);
    
    clock_gettime(CLOCK_MONOTONIC, &after);

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

    return timespec_diff(&before, &after);
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
