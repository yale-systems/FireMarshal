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
#include <sched.h>

#include "common.h"
#include "accnet_ioctl.h"
#include "iocache_ioctl.h"

#include "accnet_lib.h"
#include "iocache_lib.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#define MODE_POLLING "poll"
#define MODE_BLOCKING "block"

struct timespec timespec_diff(struct timespec *start, struct timespec *end);
struct timespec test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
                                        uint8_t payload[], uint32_t payload_size, bool debug);
struct timespec test_udp_latency_poll(struct accnet_info *accnet, uint8_t payload[], uint32_t payload_size, bool debug);
void print_deltas(uint64_t t_user_before, __u64 last_cycles[3], uint64_t t_user_after);

uint64_t time_ns[8] = {0};

static inline uint64_t rdcycle(void)
{
    uint64_t x;
    __asm__ volatile ("rdcycle %0" : "=r"(x));
    return x;
}

static void pin_to_cpu0(void) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity"); /* continue anyway */
    }
}

int main(int argc, char **argv) {
    pin_to_cpu0();

    char *accnet_filename = "/dev/accnet-misc";
    char *iocache_filename = "/dev/iocache-misc";
    int n_tests = 64;
    bool debug = false;
    uint32_t payload_size = 32*1024;
    char *src_ip = "10.0.0.2";
    char *src_mac = "0c:42:a1:a8:2d:e6";
    uint16_t src_port = 1111;
    char *dst_ip = "10.0.0.1";
    char *dst_mac = "00:0a:35:06:4d:e2";
    uint16_t dst_port = 1234;
    char *mode = MODE_POLLING;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s "
                "[--ntest N] [--mode {" MODE_POLLING "|" MODE_BLOCKING "}]"
                "[--payload-size BYTES] "
                "[--src-ip ADDR] [--src-port PORT] "
                "[--dst-ip ADDR] [--dst-port PORT] "
                "[--debug]\n", argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--ntest") == 0 && i + 1 < argc) {
            n_tests = atoi(argv[++i]);
            printf("Parsed --ntest = %d\n", n_tests);
        }
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            if (strcmp(mode, MODE_POLLING) == 0 || strcmp(mode, MODE_BLOCKING) == 0) {
                printf("Parsed --mode = %s\n", mode);
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
            printf("Parsed --payload-size = %d\n", payload_size);
        }
        else if (strcmp(argv[i], "--src-mac") == 0 && i + 1 < argc) {
            src_mac = argv[++i];
            printf("Parsed --src-mac = %s\n", src_mac);
        }
        else if (strcmp(argv[i], "--src-ip") == 0 && i + 1 < argc) {
            src_ip = argv[++i];
            printf("Parsed --src-ip = %s\n", src_ip);
        }
        else if (strcmp(argv[i], "--src-port") == 0 && i + 1 < argc) {
            src_port = (uint16_t) atoi(argv[++i]);
            printf("Parsed --src-port = %u\n", src_port);
        }
        else if (strcmp(argv[i], "--dst-mac") == 0 && i + 1 < argc) {
            dst_mac = argv[++i];
            printf("Parsed --dst-mac = %s\n", dst_mac);
        }
        else if (strcmp(argv[i], "--dst-ip") == 0 && i + 1 < argc) {
            dst_ip = argv[++i];
            printf("Parsed --dst-ip = %s\n", dst_ip);
        }
        else if (strcmp(argv[i], "--dst-port") == 0 && i + 1 < argc) {
            dst_port = (uint16_t) atoi(argv[++i]);
            printf("Parsed --dst-port = %u\n", dst_port);
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
            printf("Parsed --debug\n");
        }
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    struct accnet_info *accnet = malloc(sizeof(struct accnet_info));
    struct iocache_info *iocache = calloc(1, sizeof(*iocache));

    if (accnet_open(accnet_filename, accnet, true) < 0) {
        fprintf(stderr, "accnet_open failed\n"); 
        return 1;
    }
    if (iocache_open(iocache_filename, iocache) < 0) {
        fprintf(stderr, "iocache_open failed\n"); 
        return 1;
    }

    struct connection_info *conn = malloc(sizeof(struct connection_info));
    if (conn_from_strings_mac(conn, 0x11, src_mac, src_ip, src_port, dst_mac, dst_ip, dst_port) != 0) {
        fprintf(stderr, "conn_from_strings failed\n"); return 1;
    }

    accnet_setup_connection(accnet, conn);
    if (strcmp(mode, MODE_BLOCKING) == 0) {
        iocache_setup_connection(iocache, conn);
    }

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    /* Init rings */
    accnet_start_ring(accnet);

    long long sum_ns = 0, min_ns = 0, max_ns = 0;

    /* Run Test */
    int received_ok = 0;
    for (int i = 0; i < n_tests; i++) {
        struct timespec diff = {0, 0};

        if (strcmp(mode, MODE_POLLING) == 0) {
            diff = test_udp_latency_poll(accnet, payload, payload_size, debug); 
        }
        else if (strcmp(mode, MODE_BLOCKING) == 0) {
            diff = test_udp_latency_block(accnet, iocache, payload, payload_size, debug);
        }
        if (diff.tv_sec == 0 && diff.tv_nsec == 0) {
            continue;
        }

        long long rtt_ns = diff.tv_sec * 1e9 + diff.tv_nsec;
        if (i == 0) {
            min_ns = max_ns = rtt_ns;
        } else {
            if (rtt_ns < min_ns) min_ns = rtt_ns;
            if (rtt_ns > max_ns) max_ns = rtt_ns;
        }
        sum_ns += rtt_ns;
        ++received_ok;
        // printf("iter=%d rtt=%.3f us\n", i, rtt_ns / 1e3);
    }
    double avg_us = (sum_ns / (double)received_ok) / 1e3;
    printf("\nResults (%s): recv=%d/%d  min=%.3f us  avg=%.3f us  max=%.3f us\n",
                mode, received_ok, n_tests, min_ns/1e3, avg_us, max_ns/1e3);
    printf("Time breakdown average:\nEntry-Before: %.3f us\nPLIC-Entry: %.3f us\nIRQ-PLIC: %.3f us\nPreRead-IRQ: %.3f us\nAfter-PreRead: %.3f us\n",
            time_ns[0]/(double)received_ok/1e3, 
            time_ns[1]/(double)received_ok/1e3, 
            time_ns[2]/(double)received_ok/1e3, 
            time_ns[3]/(double)received_ok/1e3, 
            time_ns[4]/(double)received_ok/1e3);

    /* Close Accnet */
    accnet_close(accnet);
    if (strcmp(mode, MODE_BLOCKING) == 0) {
        iocache_close(iocache);
    }

    return 0;
}

struct timespec test_udp_latency_block(struct accnet_info *accnet, struct iocache_info *iocache,
uint8_t payload[], uint32_t payload_size, bool debug) 
{
    struct timespec before, mid, after;
    __u64 last_irq_ns = 0;
    __u64 last_cycles[3] = {0};
    int ret;

    /* Preparing to send the payload */
    uint32_t tx_head, tx_tail, tx_size;
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    /* Preparing to receive the payload */
    uint32_t rx_head, rx_tail, rx_size;
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    mmio_rmb();

    /* Triggering TX and starting time */
    memcpy((char *)accnet->udp_tx_buffer + tx_tail, payload, payload_size);
    uint32_t new_tx_tail = (tx_tail + payload_size) % accnet->udp_tx_size;

    clock_gettime(CLOCK_MONOTONIC, &before);
    uint64_t t_user_before = 0;
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, new_tx_tail);
    mmio_wmb();

    /* Waiting for RX */
    ret = iocache_wait_on_rx(iocache, &mid);
    uint64_t t_user_after = 0;
    clock_gettime(CLOCK_MONOTONIC, &after);

    /* Updating RX HEAD */
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, rx_tail);

    if (ret == -1) return (struct timespec){0, 0};

    /* IRQ timestamp */
    if (iocache_get_last_irq_ns(iocache, &last_irq_ns) == 0 && iocache_get_last_cycles(iocache, last_cycles) == 0) {
        struct timespec irq_ts, plic_ts, entry_ts, diff1, diff2, diff3, diff4, diff5;
        irq_ts.tv_sec  = last_irq_ns / 1000000000ULL;
        irq_ts.tv_nsec = last_irq_ns % 1000000000ULL;
        entry_ts.tv_sec  = last_cycles[1] / 1000000000ULL;
        entry_ts.tv_nsec = last_cycles[1] % 1000000000ULL;
        plic_ts.tv_sec  = last_cycles[2] / 1000000000ULL;
        plic_ts.tv_nsec = last_cycles[2] % 1000000000ULL;

        diff1 = timespec_diff(&before, &entry_ts);
        diff2 = timespec_diff(&entry_ts, &plic_ts);
        diff3 = timespec_diff(&plic_ts, &irq_ts);
        diff4 = timespec_diff(&irq_ts, &mid);
        diff5 = timespec_diff(&mid, &after);

        // printf("    IRQ-Before:     %lld.%09ld\n", (long long)diff1.tv_sec, diff1.tv_nsec);
        // printf("    PreRead-IRQ:    %lld.%09ld\n", (long long)diff2.tv_sec, diff2.tv_nsec);
        // printf("    After-PreRead:  %lld.%09ld\n", (long long)diff3.tv_sec, diff3.tv_nsec);

        time_ns[0] += diff1.tv_sec * 1e9 + diff1.tv_nsec;
        time_ns[1] += diff2.tv_sec * 1e9 + diff2.tv_nsec;
        time_ns[2] += diff3.tv_sec * 1e9 + diff3.tv_nsec;
        time_ns[3] += diff4.tv_sec * 1e9 + diff4.tv_nsec;
        time_ns[4] += diff5.tv_sec * 1e9 + diff5.tv_nsec;
    } else {
        printf("Either 'iocache_get_last_irq_ns' or 'iocache_get_last_cycles' failed\n");
    }

    return timespec_diff(&before, &after);
}

struct timespec test_udp_latency_poll(struct accnet_info *accnet, uint8_t payload[], uint32_t payload_size, bool debug)
{
    struct timespec before, after;

    int counter = 0;

    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;

    // Read buffer head/tail
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);

    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);
    mmio_rmb();

    if (debug) {
        printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);
        printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);
    }

    
    if (debug) printf("Copying mem...\n");
    memcpy((char *)accnet->udp_tx_buffer + tx_tail, payload, payload_size);
    
    uint32_t val = (tx_tail + payload_size) % accnet->udp_tx_size;
    if (debug) printf("Begin Test... (new_tail=%u) \n", val);
    
    clock_gettime(CLOCK_MONOTONIC, &before);
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, val);
    do {
        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
        mmio_rmb();
        ++counter;
        if (debug) {
            printf("Waiting for RX (new rx_tail=%u)...\n", rx_tail);
        }
    } while (rx_tail != (rx_head + payload_size) % accnet->udp_rx_size);
    
    clock_gettime(CLOCK_MONOTONIC, &after);

    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    
    // Read buffer head/tail
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);
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
            printf("%02x", ((uint8_t*)accnet->udp_rx_buffer)[i]);
        }
        printf("\n");
    }

    // Updating RX HEAD
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, rx_tail);

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

void print_deltas(uint64_t t_user_before,
                  __u64 last_cycles[3],
                  uint64_t t_user_after)
{
    double ns_per_cyc = 1e9 / 60e6;   // ~16.67 ns per cycle

    uint64_t d_user_doirq   = last_cycles[0] - t_user_before;
    uint64_t d_doirq_claim  = last_cycles[1] - last_cycles[0];
    uint64_t d_claim_isr    = last_cycles[2] - last_cycles[1];
    uint64_t d_isr_user     = t_user_after   - last_cycles[2];
    uint64_t d_user_user    = t_user_after   - t_user_before;

    // printf("user→do_irq:   %8llu cycles (~%.2f us)\n",
    //        (unsigned long long)d_user_doirq,
    //        d_user_doirq * ns_per_cyc / 1000.0);

    // printf("do_irq→claim:  %8llu cycles (~%.2f us)\n",
    //        (unsigned long long)d_doirq_claim,
    //        d_doirq_claim * ns_per_cyc / 1000.0);

    // printf("claim→ISR:     %8llu cycles (~%.2f us)\n",
    //        (unsigned long long)d_claim_isr,
    //        d_claim_isr * ns_per_cyc / 1000.0);

    // printf("ISR→user:      %8llu cycles (~%.2f us)\n",
    //        (unsigned long long)d_isr_user,
    //        d_isr_user * ns_per_cyc / 1000.0);

    printf("user→user:     %8llu cycles (~%.2f us)\n",
           (unsigned long long)d_user_user,
           d_user_user * ns_per_cyc / 1000.0);
}