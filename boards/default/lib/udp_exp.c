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

int main(int argc, char **argv) {
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
    for (int i = 0; i < n_tests; i++) {
        struct timespec diff;
        if (strcmp(mode, MODE_POLLING) == 0) {
            diff = test_udp_latency_poll(accnet, payload, payload_size, debug); 
        }
        else if (strcmp(mode, MODE_BLOCKING) == 0) {
            diff = test_udp_latency_block(accnet, iocache, payload, payload_size, debug);
        }
        long long rtt_ns = diff.tv_sec * 1e9 + diff.tv_nsec;
        if (i == 0) {
            min_ns = max_ns = rtt_ns;
        } else {
            if (rtt_ns < min_ns) min_ns = rtt_ns;
            if (rtt_ns > max_ns) max_ns = rtt_ns;
        }
        sum_ns += rtt_ns;
        printf("iter=%d rtt=%.3f us\n", i, rtt_ns / 1e3);
    }
    double avg_us = (sum_ns / (double)n_tests) / 1e3;
    printf("\nResults (%s): recv=%d  min=%.3f us  avg=%.3f us  max=%.3f us\n",
                mode, n_tests, min_ns/1e3, avg_us, max_ns/1e3);

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
    struct timespec before, after;

    /* Preparing to send the payload */
    uint32_t tx_head, tx_tail, tx_size;
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    /* Preparing to receive the payload */
    uint32_t rx_head, rx_tail, rx_size;
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    
    /* Triggering TX and starting time */
    memcpy((char *)accnet->udp_tx_buffer + tx_tail, payload, payload_size);
    uint32_t new_tx_tail = (tx_tail + payload_size) % accnet->udp_tx_size;

    
    clock_gettime(CLOCK_MONOTONIC, &before);
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, new_tx_tail);

    /* Waiting for RX */
    iocache_wait_on_rx(iocache);
    clock_gettime(CLOCK_MONOTONIC, &after);

    /* Updating RX HEAD */
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, rx_tail);

    return timespec_diff(&before, &after);
}

struct timespec test_udp_latency_poll(struct accnet_info *accnet, uint8_t payload[], uint32_t payload_size, bool debug)
{
    struct timespec before, after;

    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;

    // Read buffer head/tail
    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);

    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

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
    while (rx_tail != (rx_head + payload_size) % accnet->udp_rx_size) {
        rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
        if (debug) {
            printf("Waiting for RX (new rx_tail=%u)...\n", rx_tail);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &after);

    rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    
    // Read buffer head/tail
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    if (debug) {
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