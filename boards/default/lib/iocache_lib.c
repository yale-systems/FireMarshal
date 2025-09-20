#define _GNU_SOURCE
#include <sched.h>
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
#include <stdbool.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <inttypes.h>

#include "iocache_ioctl.h"
#include "iocache_lib.h"
#include "common.h"

static inline void print_util(uint64_t start, uint64_t end, uint64_t used) {
    if (end <= start) {
        printf("Utilization: n/a (bad window)\n");
        return;
    }
    uint64_t wall = end - start;
    double pct = (double)used / (double)wall * 100.0;   // relative to one CPU
    printf("CPU Utilization: %.1f%%  (cpu=%" PRIu64 " ns, wall=%" PRIu64 " ns)\n",
           pct, used, wall);
    fflush(stdout);
}

static int _iocache_reg_info(int fd, int index, off_t *offset, size_t *size) {
    struct iocache_ioctl_region_info region_info;
    memset(&region_info, 0, sizeof(region_info));
    region_info.argsz = sizeof(region_info);
    region_info.flags = 0;
    region_info.index = index;  // use the parameter

    if (ioctl(fd, IOCACHE_IOCTL_GET_REGION_INFO, &region_info) == -1) {
        perror("IOCACHE_IOCTL_GET_REGION_INFO ioctl failed");
        return -1;
    }

    if (offset) *offset = (off_t)region_info.offset;
    if (size)   *size   = (size_t)region_info.size;

    /* Optional: ensure page alignment for mmap */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0 && (region_info.offset & (pg - 1)) != 0) {
        fprintf(stderr, "warning: region offset not page-aligned (0x%jx)\n",
                (uintmax_t)region_info.offset);
    }
    return 0;
}


int iocache_wait_on_rx(struct iocache_info *iocache) { 

    if (iocache_is_rx_available(iocache))
        return 0;

    if (ioctl(iocache->fd, IOCACHE_IOCTL_WAIT_TIMEOUT) == -1) {
        perror("IOCACHE_IOCTL_WAIT_TIMEOUT ioctl failed");
        return -1;
    } 

    /* Return -1 if it was a timeout */
    return (iocache_is_rx_available(iocache)) ? 0 : -1;
}

int iocache_wait_on_txcomp(struct iocache_info *iocache) {
    return 0;
}

int iocache_get_last_irq_ns(struct iocache_info *iocache, __u64 *ns) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_GET_LAST_IRQ_NS, ns) == -1) {
        perror("IOCACHE_IOCTL_GET_LAST_IRQ_NS ioctl failed");
        return -1;
    }
    return 0;
}

int iocache_get_last_ktimes(struct iocache_info *iocache, __u64 ktimes[3]) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_GET_KTIMES, ktimes) == -1) {
        perror("IOCACHE_IOCTL_GET_KTIMES ioctl failed");
        return -1;
    }
    return 0;
}

int iocache_print_proc_util(struct iocache_info *iocache) {
    __u64 *ns = malloc(sizeof(__u64) * 3);

    if (ioctl(iocache->fd, IOCACHE_IOCTL_GET_PROC_UTIL, ns) == -1) {
        perror("IOCACHE_IOCTL_GET_PROC_UTIL ioctl failed");
        return -1;
    }
    print_util(ns[0], ns[1], ns[2]);
    return 0;
}

int iocache_start_scheduler(struct iocache_info *iocache) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_START_SCHEDULER) == -1) {
        perror("IOCACHE_IOCTL_RUN_SCHEDULER ioctl failed");
        return -1;
    }
    iocache->scheduler_enabled = true;
    return 0;
}

int iocache_stop_scheduler(struct iocache_info *iocache) {
    if (iocache->scheduler_enabled) {
        if (ioctl(iocache->fd, IOCACHE_IOCTL_STOP_SCHEDULER, &iocache->row) == -1) {
            perror("IOCACHE_IOCTL_STOP_SCHEDULER ioctl failed");
            return -1;
        }
        iocache->scheduler_enabled = false;
    }
    return 0;
}

void iocache_setup_connection(struct iocache_info *iocache, struct connection_info *entry) {
    int row = iocache->row;
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    entry->protocol);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      entry->src_ip);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    entry->src_port);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      entry->dst_ip);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    entry->dst_port);
    reg_write16(iocache->regs, IOCACHE_REG_CONN_ID(row),     entry->src_port);
    mmio_wmb();
}

void iocache_clear_connection(struct iocache_info *iocache) {
    int row = iocache->row;
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    0);
    reg_write16(iocache->regs, IOCACHE_REG_CONN_ID(row),     0);
    mmio_wmb();
}

static int _iocache_reserve_ring(struct iocache_info *iocache) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_RESERVE_RING, &iocache->row) == -1) {
        perror("IOCACHE_IOCTL_RESERVE_RING ioctl failed");
        close(iocache->fd);
        return -1;
    }
    iocache->is_ring_reserved = true;

    return 0;
}

static int _iocache_free_ring(struct iocache_info *iocache) {
    if (iocache->is_ring_reserved) {
        if (ioctl(iocache->fd, IOCACHE_IOCTL_FREE_RING) == -1) {
            perror("IOCACHE_IOCTL_FREE_RING ioctl failed");
            close(iocache->fd);
            return -1;
        }
        iocache->is_ring_reserved = false;
    }
    return 0;
}

static int _iocache_do_mmap(struct iocache_info *iocache) {
    uintptr_t p;
    int udp_tx_buffer_index;
    int udp_rx_buffer_index;

    udp_tx_buffer_index = 2*iocache->row + 1;
    udp_rx_buffer_index = 2*iocache->row + 2;

    /* mmap registers */
    iocache->regs = (volatile uint8_t *)mmap(NULL, iocache->regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(0));
    if (iocache->regs == MAP_FAILED) {
        perror("mmap regs failed");
        return -1;
    }

    iocache->udp_tx_buffer = mmap(NULL, iocache->udp_tx_size + (ALIGN - 1), 
                                    PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(udp_tx_buffer_index));
    if (iocache->udp_tx_buffer == MAP_FAILED) {
        perror("mmap udp tx");
        return -1;
    }
    p = (uintptr_t)iocache->udp_tx_buffer;
    iocache->udp_tx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    iocache->udp_rx_buffer = mmap(NULL, iocache->udp_rx_size + (ALIGN - 1), 
                                    PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(udp_rx_buffer_index));
    if (iocache->udp_rx_buffer == MAP_FAILED) {
        perror("mmap udp rx");
        return -1;
    }
    p = (uintptr_t)iocache->udp_rx_buffer;
    iocache->udp_rx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    return 0;
}

static int _iocache_do_munmap(struct iocache_info *iocache) {
    munmap((void *)(uintptr_t) iocache->regs, iocache->regs_size);
    munmap((void *)(uintptr_t) iocache->udp_rx_buffer, iocache->udp_rx_size);
    munmap((void *)(uintptr_t) iocache->udp_tx_buffer, iocache->udp_tx_size);

    return 0;
}

int iocache_open(char *file, struct iocache_info *iocache) {
    iocache->udp_tx_size = IOCACHE_UDP_RING_SIZE;
    iocache->udp_rx_size = IOCACHE_UDP_RING_SIZE;

    iocache->fd = open(file, O_RDWR | O_SYNC);
    if (iocache->fd < 0) {
        perror("open");
        return -1;
    }

    iocache->cpu = sched_getcpu();

    if (_iocache_reg_info(iocache->fd, 0, &iocache->regs_offset, &iocache->regs_size) != 0) {
        close(iocache->fd);
        return -1;
    }

    if (_iocache_reserve_ring(iocache) != 0) {
        close(iocache->fd);
        return -1;
    }

    if (_iocache_do_mmap(iocache) != 0) {
        close(iocache->fd);
        return -1;
    }

    return 0;
}

int iocache_close(struct iocache_info *iocache) {
    if (iocache) {
        iocache_stop_scheduler(iocache);
        
        iocache_clear_connection(iocache);

        _iocache_free_ring(iocache);

        _iocache_do_munmap(iocache);
        
        close(iocache->fd);
    }
    return 0;
}