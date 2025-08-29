#define __USE_GNU
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
#include <stdatomic.h>

#include <sched.h>
#include <sys/resource.h>

#include "common.h"
#include "accnet_ioctl.h"
#include "iocache_ioctl.h"

#include "accnet_lib.h"
#include "iocache_lib.h"

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// Lock memory to avoid major page faults during timing critical work
static void lock_memory_or_warn(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall (non-fatal)");
    }
}

// Set calling thread to SCHED_FIFO with given priority (1..99)
static int set_rt_fifo_prio(int prio) {
    struct sched_param sp = { .sched_priority = prio };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        return -errno;
    }
    return 0;
}

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

static _Atomic int64_t t_start_ns = -1;
static _Atomic int64_t t_end_ns   = -1;

static inline int64_t timespec_to_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
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

static void *send_thread(void *arg) {
    send_args_t *A = (send_args_t *)arg;
    struct accnet_info  *accnet  = A->accnet;
    struct iocache_info *iocache = A->iocache;
    size_t payload_size          = A->payload_size;
    struct timespec ts;

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    uint32_t tx_head, tx_tail, tx_size;
    tx_head = accnet_get_tx_head(accnet); 
    tx_tail = accnet_get_tx_tail(accnet); 
    tx_size = accnet_get_tx_size(accnet); 

    memcpy((char *)accnet->udp_tx_buffer + tx_tail, payload, payload_size);
    __sync_synchronize();

    uint32_t val = (tx_tail + payload_size) % accnet->udp_tx_size;
    
    clock_gettime(CLOCK_MONOTONIC, &ts);

    accnet_set_tx_tail(accnet, val);
    t_start_ns = timespec_to_ns(&ts);
    
    return NULL;
}

static void *recv_thread(void *arg) {
    recv_args_t *A = (recv_args_t *)arg;
    struct accnet_info  *accnet  = A->accnet;
    struct iocache_info *iocache = A->iocache;
    struct timespec ts;
    int res;

    uint32_t rx_head, rx_tail, rx_size;
    rx_size = accnet_get_rx_size(accnet);
    rx_head = accnet_get_rx_head(accnet);

    res = iocache_wait_on_rx(iocache, NULL);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t_end_ns = timespec_to_ns(&ts);

    rx_tail = accnet_get_rx_tail(accnet);
    int size = (rx_tail > rx_head) ? rx_tail - rx_head : rx_size - (rx_head - rx_tail);

    // Updating RX HEAD
    accnet_set_rx_head(accnet, rx_tail);
    
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
            fprintf(stderr, "Usage: %s [--bytes N] [--payload-size N] [--threads N]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 1;
        }
    }

    lock_memory_or_warn();

    // Try for max RT priority (99). If this fails, consider printing why and exiting or falling back.
    int err = set_rt_fifo_prio(99);
    if (err) {
        fprintf(stderr, "sched_setscheduler(SCHED_FIFO,99) failed: %s\n", strerror(-err));
        // Optional fallback: try a negative nice within SCHED_OTHER
        if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
            perror("setpriority");
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
    accnet_start_ring(accnet);

    // Configure a connection in IOCache (adjust to your needs)
    if (conn_from_strings(conn, 0x11, "10.0.0.1", 1234, "10.0.0.2", 1111) != 0) {
        fprintf(stderr, "conn_from_strings failed\n"); 
        return 1;
    }

    iocache_setup_connection(iocache, conn);

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // Explicitly use the scheduling attributes we set here (don't inherit)
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    // SCHED_FIFO 99
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    struct sched_param sp = { .sched_priority = 99 };
    pthread_attr_setschedparam(&attr, &sp);

    // Spawn receiver thread
    pthread_t *recv_tids    = calloc(1, sizeof(*recv_tids));
    recv_args_t *recv_args  = calloc(1, sizeof(*recv_args));
    recv_args[0].accnet     = accnet;
    recv_args[0].iocache    = iocache;
    rc = pthread_create(&recv_tids[0], &attr, recv_thread, &recv_args[0]);
    if (rc) { errno = rc; perror("pthread_create recv"); return 1; }
    printf("waiting for rx thread to start...\n");
    sleep(0.2);

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
        rc = pthread_create(&send_tids[i], &attr, send_thread, &send_args[i]);
        if (rc) { errno = rc; perror("pthread_create send"); return 1; }
    }

    pthread_attr_destroy(&attr);
    
    // Wait for all threads to finish
    for (int i = 0; i < threads; ++i) {
        pthread_join(send_tids[i], NULL);
    }
    printf("TX threads done\n");

    pthread_join(recv_tids[0], NULL);
    printf("RX threads done\n");

    int64_t delta_ns = t_end_ns - t_start_ns;
    printf("*** One-shot latency:\n");
    printf("  %ld ns (%.3f us, %.3f ms)\n",
        (long)delta_ns, delta_ns/1000.0, delta_ns/1e6);
    printf("\nstart: %ld, end: %ld\n", t_start_ns, t_end_ns);
    
    free(send_args);
    free(send_tids);
    free(recv_args);
    free(recv_tids);

    accnet_close(accnet);
    iocache_close(iocache);

    return 0;
}