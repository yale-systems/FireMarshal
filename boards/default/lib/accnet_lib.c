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

#include "accnet_ioctl.h"
#include "accnet_lib.h"
#include "iocache_lib.h"
#include "common.h"

static int _accnet_ioctl(int fd, int index, off_t *offset, size_t *size) {
    struct accnet_ioctl_region_info region_info;
    memset(&region_info, 0, sizeof(region_info));
    region_info.argsz = sizeof(region_info);
    region_info.flags = 0;
    region_info.index = index;  // use the parameter

    if (ioctl(fd, ACCNET_IOCTL_GET_REGION_INFO, &region_info) == -1) {
        perror("ACCNET_IOCTL_GET_REGION_INFO ioctl failed");
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

/* Initialize udp on nic registers */
static void _init_regs(struct accnet_info *accnet, int row) {

    // printf("Init regs for row %d\n", accnet->iocache->row);

	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_MTU, 1472);
	reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_SRC, 0x112233445566); 
	reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_DST, 0x0c42a1a82de6);// 0c:42:a1:a8:2d:e6
	reg_write8 (accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TOS, 0);
	reg_write8 (accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TTL, 64);
	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_ID, 0);

    reg_write32(accnet->iocache->regs, IOCACHE_REG_RX_RING_SIZE(row),   0);
    reg_write32(accnet->iocache->regs, IOCACHE_REG_TX_RING_SIZE(row),   0);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD(row), 0);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row), 0);
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(row), 0);
	reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row), 0);
    reg_write32(accnet->iocache->regs, IOCACHE_REG_RX_RING_SIZE(row),   accnet->iocache->udp_rx_size);
    reg_write32(accnet->iocache->regs, IOCACHE_REG_TX_RING_SIZE(row),   accnet->iocache->udp_tx_size);

    if (reg_read32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL(row)) != 0) {
        exit(0);
    }
    if (reg_read32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL(row)) != 0) {
        exit(0);
    }
}

static void _deinit_regs(struct accnet_info *accnet, int row) {
    
}

int accnet_setup_connection(struct accnet_info *accnet, struct connection_info *connection) {
    if (accnet) {
        // reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_SRC,       connection->src_ip);
        // reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_DST,       connection->dst_ip);
        // reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_SRC_PORT, connection->src_port);
        // reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_DST_PORT, connection->dst_port);
        if (connection->src_mac) {
            reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_SRC, connection->src_mac);
        }
        if (connection->dst_mac) {
            reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_DST, connection->dst_mac);
        }
        return 0;
    }
    else {
        return -1;
    }
}

int accnet_start_ring(struct accnet_info *accnet) {
    if (accnet) {
        // reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE, accnet->udp_tx_size);
        // reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE, accnet->udp_rx_size);
        return 0;
    }
    else {
        return -1;
    }
}

uint64_t accnet_get_outside_ticks(struct accnet_info *accnet) {
    int row = accnet->iocache->row;
    uint64_t rx_timestamp = reg_read64(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_LAST_TIMESTAMP(row));
    uint64_t tx_timestamp = reg_read64(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_LAST_TIMESTAMP(row));

    if (rx_timestamp <= tx_timestamp) {
        // printf("Warning: bad timestamps -- tx=%lu , rx=%lu\n", tx_timestamp, rx_timestamp);
        return 0;
    }
    return (uint64_t)(rx_timestamp - tx_timestamp);
}

size_t accnet_send(struct accnet_info *accnet, void *buffer, size_t len) {
    struct iocache_info *iocache = accnet->iocache;

    uint32_t tx_head, tx_tail, tx_size;

    tx_head = accnet_get_tx_head(accnet); 
    tx_tail = accnet_get_tx_tail(accnet); 
    tx_size = iocache->udp_tx_size;

    uint32_t avail = (tx_tail > tx_head) ? tx_tail - tx_head : tx_size - (tx_head - tx_size);
    uint32_t val = (tx_tail + len) % tx_size;

    memcpy((char *)iocache->udp_tx_buffer + tx_tail, buffer, len);
    __sync_synchronize();

    accnet_set_tx_tail(accnet, val);

    return len;
}

int accnet_open(char *file, struct accnet_info *accnet, struct iocache_info *iocache, bool do_init) {
    uintptr_t p;

    if (!iocache) {
        perror("iocache not opened");
        return -1;
    }

    accnet->fd = open(file, O_RDWR | O_SYNC);
    if (accnet->fd < 0) {
        perror("open");
        return -1;
    }

    accnet->iocache = iocache;

    if (_accnet_ioctl(accnet->fd, 0, &accnet->regs_offset, &accnet->regs_size) != 0 ||
    _accnet_ioctl(accnet->fd, 1, &accnet->udp_tx_offset, &accnet->udp_tx_regs_size) != 0 ||
    _accnet_ioctl(accnet->fd, 2, &accnet->udp_rx_offset, &accnet->udp_rx_regs_size) != 0) 
    {
        close(accnet->fd);
        return -1;
    }

    /* mmap registers and dma memory */
    accnet->regs = (volatile uint8_t *)mmap(NULL, accnet->regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, accnet->fd, MAP_INDEX(0));
    if (accnet->regs == MAP_FAILED)
    {
        perror("mmap regs failed");
        close(accnet->fd);
        return -1;
    }

    accnet->udp_tx_regs = (volatile uint8_t *)mmap(NULL, accnet->udp_tx_regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, accnet->fd, MAP_INDEX(1));
    if (accnet->udp_tx_regs == MAP_FAILED)
    {
        perror("udp_tx_regs mmap regs failed");
        close(accnet->fd);
        return -1;
    }
    
    accnet->udp_rx_regs = (volatile uint8_t *)mmap(NULL, accnet->udp_rx_regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, accnet->fd, MAP_INDEX(2));
    if (accnet->udp_rx_regs == MAP_FAILED)
    {
        perror("udp_rx_regs mmap regs failed");
        close(accnet->fd);
        return -1;
    }

    if (do_init) {
        _init_regs(accnet, iocache->row);
    }

    return 0;
}

/* Cleanup */
int accnet_close(struct accnet_info *accnet) {
    _deinit_regs(accnet, accnet->iocache->row);
    munmap((void *)(uintptr_t) accnet->regs, accnet->regs_size);
    munmap((void *)(uintptr_t) accnet->udp_tx_regs, accnet->udp_tx_regs_size);
    munmap((void *)(uintptr_t) accnet->udp_rx_regs, accnet->udp_rx_regs_size);
    close(accnet->fd);

    return 0;
}