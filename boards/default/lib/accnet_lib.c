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
#include "common.h"

static int do_ioctl(int fd, int index, off_t *offset, size_t *size) {
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

    /* Print safely: name may not be NUL-terminated */
    printf("region index=%d, offset=0x%jx, size=0x%zx, name='%.*s', next=%u, flags=0x%x\n",
           index,
           (uintmax_t)region_info.offset,
           (size_t)region_info.size,
           (int)sizeof region_info.name, region_info.name,
           region_info.next,
           region_info.flags);

    /* Optional: ensure page alignment for mmap */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0 && (region_info.offset & (pg - 1)) != 0) {
        fprintf(stderr, "warning: region offset not page-aligned (0x%jx)\n",
                (uintmax_t)region_info.offset);
    }
    return 0;
}

/* Initialize udp on nic registers */
static void init_regs(struct accnet_info *accnet) {
    // Contor registers
	reg_write32(accnet->regs, ACCNET_CTRL_FILTER_PORT, 1234);
	reg_write32(accnet->regs, ACCNET_CTRL_FILTER_IP,   0x0A000001); // 10.0.0.1
	// RX
	reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_SIZE, 0);
	reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, 0);
	reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_TAIL, 0);
	// TX
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_SIZE, 0);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_HEAD, 0);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, 0);

	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_MTU, 1472);
	reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_SRC, 0x112233445566); 
	reg_write64(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_DST, 0x0c42a1a82de6);// 0c:42:a1:a8:2d:e6
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_SRC, 0x0a0b0c0d);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_DST, 0x0A000001);
	reg_write8 (accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TOS, 0);
	reg_write8 (accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TTL, 64);
	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_ID, 0);
	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_SRC_PORT, 1111);
	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_DST_PORT, 1234);
	// reg_write8(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_CSUM, 1500);
}

int accnet_open(char *file, struct accnet_info *accnet, bool do_init) {
    uintptr_t p;

    const size_t ALIGN = 64;

    accnet->udp_tx_size = 256 * 1024;
    accnet->udp_rx_size = 256 * 1024;

    accnet->fd = open(file, O_RDWR | O_SYNC);
    if (accnet->fd < 0) {
        perror("open");
        return -1;
    }

    if (do_ioctl(accnet->fd, 0, &accnet->regs_offset, &accnet->regs_size) != 0 ||
    do_ioctl(accnet->fd, 1, &accnet->udp_tx_offset, &accnet->udp_tx_regs_size) != 0 ||
    do_ioctl(accnet->fd, 2, &accnet->udp_rx_offset, &accnet->udp_rx_regs_size) != 0) 
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

    accnet->udp_tx_buffer = mmap(NULL, accnet->udp_tx_size + (ALIGN - 1), PROT_READ | PROT_WRITE, MAP_SHARED, accnet->fd, MAP_INDEX(3));
    if (accnet->udp_tx_buffer == MAP_FAILED) {
        perror("mmap udp tx");
        close(accnet->fd);
        return -1;
    }
    p = (uintptr_t)accnet->udp_tx_buffer;
    accnet->udp_tx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    accnet->udp_rx_buffer = mmap(NULL, accnet->udp_rx_size + (ALIGN - 1), PROT_READ | PROT_WRITE, MAP_SHARED, accnet->fd, MAP_INDEX(4));
    if (accnet->udp_rx_buffer == MAP_FAILED) {
        perror("mmap udp rx");
        munmap((void *) accnet->udp_tx_regs, accnet->udp_tx_regs_size);
        close(accnet->fd);
        return -1;
    }
    p = (uintptr_t)accnet->udp_rx_buffer;
    accnet->udp_rx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    if (do_init) {
        init_regs(accnet);
    }

    return 0;
}

/* Cleanup */
int accnet_close(struct accnet_info *accnet) {
    munmap((void *) accnet->regs, accnet->regs_size);
    munmap((void *) accnet->udp_tx_regs, accnet->udp_tx_regs_size);
    munmap((void *) accnet->udp_rx_regs, accnet->udp_rx_regs_size);
    munmap((void *) accnet->udp_rx_buffer, accnet->udp_rx_size);
    munmap((void *) accnet->udp_tx_buffer, accnet->udp_tx_size);
    close(accnet->fd);

    return 0;
}