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

struct timespec timespec_diff(struct timespec *start, struct timespec *end);
struct timespec test_udp_latency(struct accnet_info *info, uint8_t payload[], uint32_t payload_size, bool debug);

int main(int argc, char **argv) {
    char *accnet_dev = "/dev/accnet-misc";
    struct accnet_info *info = malloc(sizeof(struct accnet_info));
    int n_tests = 64;
    bool debug = false;
    uint32_t payload_size = 32*1024;
    long total = 0;
    double avg = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--ntest N] [--debug STR]\n", argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--ntest") == 0 && i + 1 < argc) {
            n_tests = atoi(argv[++i]);
            printf("Parsed --ntest = %d\n", n_tests);
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
        else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
            printf("Parsed --debug\n");
        }
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    accnet_open(accnet_dev, info, true);

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    /* Init rings */
    reg_write32(info->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE, info->udp_tx_size);
    reg_write32(info->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE, info->udp_rx_size);

    /* Run Test */
    for (int i = 0; i < n_tests; i++) {
        struct timespec diff = test_udp_latency(info, payload, payload_size, debug); 
        long time_diff = diff.tv_sec * 1e9 + diff.tv_nsec;
        total += time_diff;
        printf("*** Trial %d: %ld ns\n", i, time_diff);
    }
    avg = (double) total / n_tests;
    printf("*** Overall:\nAverage delay: %0.2f ns\n", avg);

    /* Close Accnet */
    accnet_close(info);

    return 0;
}

struct timespec test_udp_latency(struct accnet_info *info, uint8_t payload[], uint32_t payload_size, bool debug)
{
    struct timespec before, after;

    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;

    // Read buffer head/tail
    rx_head = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_tail = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    rx_size = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);

    tx_head = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    if (debug) {
        printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);
        printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);
    }

    if (debug) printf("Copying mem...\n");
    memcpy((char *)info->udp_tx_buffer + tx_tail, payload, payload_size);

    uint32_t val = (tx_tail + payload_size) % info->udp_tx_size;
    if (debug) printf("Begin Test... (new_tail=%u) \n", val);

    clock_gettime(CLOCK_MONOTONIC, &before);

    reg_write32(info->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, val);
    while (rx_tail != (rx_head + payload_size) % info->udp_rx_size) {
        rx_tail = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
        if (debug) {
            printf("Waiting for RX (new rx_tail=%u)...\n", rx_tail);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &after);

    rx_head = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_size = reg_read32(info->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    
    // Read buffer head/tail
    tx_head = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(info->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

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
            printf("%02x", ((uint8_t*)info->udp_rx_buffer)[i]);
        }
        printf("\n");
    }

    // Updating RX HEAD
    reg_write32(info->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, rx_tail);

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