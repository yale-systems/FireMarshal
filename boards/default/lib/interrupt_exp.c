#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
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

static inline int ts_printf(const char *fmt, ...)
{
    int rc;
    va_list ap;
    flockfile(stdout);            // lock stdout for this thread
    va_start(ap, fmt);
    rc = vfprintf(stdout, fmt, ap);
    va_end(ap);
    funlockfile(stdout);          // unlock
    return rc;
}

typedef struct {
    struct accnet_info      *accnet;
    struct iocache_info     *iocache;
    struct connection_info  *conn;  // already-parsed IP/ports (host order or net order per your choice)
    size_t                  total_bytes;
    size_t                  payload_size;
} send_args_t;

typedef struct {
    struct accnet_info      *accnet;
    struct iocache_info     *iocache;
} recv_args_t;

static void *send_thread(void *arg) {
    send_args_t *A = (send_args_t *)arg;
    struct accnet_info  *accnet  = A->accnet;
    struct iocache_info *iocache = A->iocache;
    size_t payload_size          = A->payload_size;

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    uint32_t tx_head, tx_tail, tx_size;
    tx_head = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    memcpy((char *)accnet->udp_tx_buffer + tx_tail, payload, payload_size);

    uint32_t val = (tx_tail + payload_size) % accnet->udp_tx_size;
    ts_printf("[TX-Thread] Begin Send... (new_tail=%u, old_tail=%u, old_head=%u) \n", val, tx_tail, tx_head);
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, val);
    
    return NULL;
}

static void *recv_thread(void *arg) {
    recv_args_t *A = (recv_args_t *)arg;
    struct accnet_info  *accnet  = A->accnet;
    struct iocache_info *iocache = A->iocache;

    int res;

    uint32_t rx_head, rx_tail, rx_size;
    rx_size = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);

    ts_printf("[RX-Thread] Starting RX...\n");
    for (;;) {
        if (iocache_is_rx_available(iocache)) {
            rx_head = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
            rx_tail = reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
            int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);
            ts_printf("[RX-Thread] Received %d bytes of data\n", size);

            // Updating RX HEAD
            reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, rx_tail);

            break;
        }
        else {
            ts_printf("[RX-Thread] No data. Waiting...\n");
            res = iocache_wait_on_rx(iocache);
        }
    }
    
    return NULL;
}

int main(int argc, char **argv) {
    char *accnet_filename = "/dev/accnet-misc";
    char *iocache_filename = "/dev/iocache-misc";
    int threads      = 1;
    size_t total_bytes  = 1024;         // not used now
    size_t payload_size = 1024;         // default: 1 KiB

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bytes") && i+1 < argc) {
            total_bytes  = strtoull(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--payload-size") && i+1 < argc) {
            payload_size = strtoull(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--threads") && i+1 < argc) {
            threads      = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: %s [--bytes N] [--payload N] [--threads N]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    int rc;
    struct accnet_info  *accnet  = calloc(1, sizeof(*accnet));
    struct iocache_info *iocache = calloc(1, sizeof(*iocache));
    struct connection_info *conn = malloc(sizeof(struct connection_info));
    if (!accnet || !iocache || !conn) { 
        perror("calloc"); return 1; 
    }

    if (accnet_open(accnet_filename, accnet, true) < 0) {
        fprintf(stderr, "accnet_open failed\n"); 
        return 1;
    }
    if (iocache_open(iocache_filename, iocache) < 0) {
        fprintf(stderr, "iocache_open failed\n"); 
        return 1;
    }
    
    /* Init rings */
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE, accnet->udp_tx_size);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE, accnet->udp_rx_size);

    // Configure a connection in IOCache (adjust to your needs)
    if (conn_from_strings(conn, 0x11, "10.0.0.1", 1234, "10.0.0.2", 1111) != 0) {
        fprintf(stderr, "conn_from_strings failed\n"); return 1;
    }

    iocache_setup_connection(iocache, conn);

    // Spawn receiver thread
    pthread_t *recv_tids    = calloc(1, sizeof(*recv_tids));
    recv_args_t *recv_args  = calloc(1, sizeof(*recv_args));
    recv_args[0].accnet     = accnet;
    recv_args[0].iocache    = iocache;
    rc = pthread_create(&recv_tids[0], NULL, recv_thread, &recv_args[0]);
    if (rc) { 
        errno = rc; 
        perror("pthread_create"); 
        return 1; 
    }
    printf("waiting for rx thread to start...\n");
    sleep(1);

    // Spawn sender thread(s)
    if (threads < 1) threads = 1;
    pthread_t *send_tids = calloc(threads, sizeof(*send_tids));
    send_args_t *send_args = calloc(threads, sizeof(*send_args));
    if (!send_tids || !send_args) { 
        perror("calloc"); return 1; 
    }

    size_t per_thread = total_bytes / (size_t)threads;
    size_t remainder  = total_bytes % (size_t)threads;

    for (int i = 0; i < threads; ++i) {
        send_args[i].accnet        = accnet;
        send_args[i].iocache       = iocache;
        send_args[i].total_bytes   = per_thread + (i == 0 ? remainder : 0);
        send_args[i].payload_size  = payload_size;
        send_args[i].conn          = conn;
        rc = pthread_create(&send_tids[i], NULL, send_thread, &send_args[i]);
        if (rc) { 
            errno = rc; 
            perror("pthread_create"); 
            return 1;
        }
    }
    
    // Wait for all threads to finish
    for (int i = 0; i < threads; ++i) {
        pthread_join(send_tids[i], NULL);
    }
    printf("TX threads done\n");

    pthread_join(recv_tids[0], NULL);
    printf("RX threads done\n");


    free(send_args);
    free(send_tids);
    free(recv_args);
    free(recv_tids);

    accnet_close(accnet);
    iocache_close(iocache);

    return 0;
}