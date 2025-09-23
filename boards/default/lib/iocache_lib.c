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

static int _iocache_ioctl(int fd, int index, off_t *offset, size_t *size) {
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

    // /* Print safely: name may not be NUL-terminated */
    // printf("region index=%d, offset=0x%jx, size=0x%zx, name='%.*s', next=%u, flags=0x%x\n",
    //        index,
    //        (uintmax_t)region_info.offset,
    //        (size_t)region_info.size,
    //        (int)sizeof region_info.name, region_info.name,
    //        region_info.next,
    //        region_info.flags);
    // fflush(stdout);

    /* Optional: ensure page alignment for mmap */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0 && (region_info.offset & (pg - 1)) != 0) {
        fprintf(stderr, "warning: region offset not page-aligned (0x%jx)\n",
                (uintmax_t)region_info.offset);
    }
    return 0;
}

static inline void _iocache_enable_interrupts_rx(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_INTMASK_RX(iocache->cpu),     0x1);
}

static inline void _iocache_disable_interrupts_rx(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_INTMASK_RX(iocache->cpu),     0x0);
}

static inline void iocache_set_rx_suspended(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_RX_SUSPENDED(iocache->row),     0x1);
}

static inline void iocache_clear_rx_suspended(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_RX_SUSPENDED(iocache->row),     0x0);
}

static inline void iocache_set_txcomp_suspended(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_TXCOMP_SUSPENDED(iocache->row),     0x1);
}

static inline void iocache_clear_txcomp_suspended(struct iocache_info *iocache) {
    reg_write8(iocache->regs, IOCACHE_REG_TXCOMP_SUSPENDED(iocache->row),     0x0);
}

int iocache_wait_on_rx(struct iocache_info *iocache) { 
    struct epoll_event out;
    uint64_t cnt;

    if (iocache_is_rx_available(iocache))
        return 0;

    // printf("Going into waiting syscall...\n");

    iocache_set_rx_suspended(iocache);
    _iocache_enable_interrupts_rx(iocache);

    if (ioctl(iocache->fd, IOCACHE_IOCTL_WAIT_READY) == -1) {
        iocache_clear_rx_suspended(iocache);
        perror("IOCACHE_IOCTL_WAIT_READY ioctl failed");
        return -1;
    } 

    // printf("Back from waiting syscall...\n");


    iocache_clear_rx_suspended(iocache);

    /* We have to return -1 if it was a timeout */
    return (iocache_is_rx_available(iocache)) ? 0 : -1;
}

int iocache_wait_on_txcomp(struct iocache_info *iocache) {
    struct epoll_event out;
    uint64_t cnt;

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
    printf("start=%lu, end=%lu, usage=%lu\n");
    print_util(ns[0], ns[1], ns[2]);
    return 0;
}

int iocache_start_scheduler(struct iocache_info *iocache) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_RUN_SCHEDULER) == -1) {
        perror("IOCACHE_IOCTL_RUN_SCHEDULER ioctl failed");
        return -1;
    }
    return 0;
}

int iocache_stop_scheduler(struct iocache_info *iocache) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_STOP_SCHEDULER, &iocache->row) == -1) {
        perror("IOCACHE_IOCTL_STOP_SCHEDULER ioctl failed");
        return -1;
    }
    return 0;
}

int _iocache_reserve_ring(struct iocache_info *iocache, int row) {

    if (ioctl(iocache->fd, IOCACHE_IOCTL_RESERVE_RING, &row) == -1) {
        perror("IOCACHE_IOCTL_RESERVE_RING ioctl failed");
        return -1;
    }
    // printf("** Reserved row=%d\n", row);
    iocache->row = row;

    return 0;
}

int _iocache_free_ring(struct iocache_info *iocache) {
    if (ioctl(iocache->fd, IOCACHE_IOCTL_FREE_RING) == -1) {
        perror("IOCACHE_IOCTL_FREE_RING ioctl failed");
        return -1;
    }
    return 0;
}

int iocache_open(char *file, struct iocache_info *iocache, int row) {
    uintptr_t p;
    iocache->udp_tx_size = 8 * 1024;
    iocache->udp_rx_size = 8 * 1024;

    const size_t ALIGN = 64;

    iocache->fd = open(file, O_RDWR | O_SYNC);
    if (iocache->fd < 0) {
        perror("open");
        return -1;
    }

    iocache->cpu = sched_getcpu();

    if (_iocache_ioctl(iocache->fd, 0, &iocache->regs_offset, &iocache->regs_size) != 0) {
        close(iocache->fd);
        return -1;
    }

    iocache->efd = eventfd(0, EFD_NONBLOCK);

    // if (ioctl(iocache->fd, IOCACHE_IOCTL_GET_AVAIL_RING, &iocache->row) == -1) {
    //     perror("IOCACHE_IOCTL_GET_AVAIL_RING ioctl failed");
    //     close(iocache->efd);
    //     close(iocache->fd);
    //     return -1;
    // }
    // else {
    //     printf("IOCache got row %d\n", iocache->row);
    // }

    if (ioctl(iocache->fd, IOCACHE_IOCTL_SET_EVENTFD, &iocache->efd) == -1) {
        perror("IOCACHE_IOCTL_SET_EVENTFD ioctl failed");
        close(iocache->efd);
        close(iocache->fd);
        return -1;
    }

    // iocache->row = 20;
    _iocache_reserve_ring(iocache, row);


    iocache->ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = iocache->efd};
    epoll_ctl(iocache->ep, EPOLL_CTL_ADD, iocache->efd, &ev);

    /* mmap registers */
    iocache->regs = (volatile uint8_t *)mmap(NULL, iocache->regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(0));
    if (iocache->regs == MAP_FAILED) {
        perror("mmap regs failed");
        close(iocache->efd);
        close(iocache->fd);
        return -1;
    }

    iocache->udp_tx_buffer = mmap(NULL, iocache->udp_tx_size + (ALIGN - 1), 
                                    PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(2*iocache->row + 1));
    if (iocache->udp_tx_buffer == MAP_FAILED) {
        perror("mmap udp tx");
        close(iocache->fd);
        return -1;
    }
    p = (uintptr_t)iocache->udp_tx_buffer;
    iocache->udp_tx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    iocache->udp_rx_buffer = mmap(NULL, iocache->udp_rx_size + (ALIGN - 1), 
                                    PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(2*iocache->row + 2));
    if (iocache->udp_rx_buffer == MAP_FAILED) {
        perror("mmap udp rx");
        munmap((void *)(uintptr_t) iocache->regs, iocache->regs_size);
        close(iocache->fd);
        return -1;
    }
    p = (uintptr_t)iocache->udp_rx_buffer;
    iocache->udp_rx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    _iocache_enable_interrupts_rx(iocache);

    return 0;
}

int iocache_close(struct iocache_info *iocache) {
    if (iocache) {
        int neg1 = -1;
        ioctl(iocache->fd, IOCACHE_IOCTL_SET_EVENTFD, &neg1); // disarm in driver

        // _iocache_disable_interrupts_rx(iocache);

        // iocache_stop_scheduler(iocache);

        iocache_clear_rx_suspended(iocache);
        iocache_clear_connection(iocache);
    
        munmap((void *)(uintptr_t) iocache->regs, iocache->regs_size);

        munmap((void *)(uintptr_t) iocache->udp_rx_buffer, iocache->udp_rx_size);
        munmap((void *)(uintptr_t) iocache->udp_tx_buffer, iocache->udp_tx_size);

        _iocache_free_ring(iocache);

        close(iocache->ep);
        close(iocache->efd);
        close(iocache->fd);
    }
    return 0;
}