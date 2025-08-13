#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "accnet_ioctl.h"
#include "mynic.h"

#include <unistd.h>
#include <errno.h>
#include <string.h>  
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>

#define DEBUG 0

const uint32_t buffer_size = 1400;
const uint32_t mtu = 1472;

void test_nic_udp(void* udp_tx_buffer, void* udp_rx_buffer, volatile uint8_t *regs, 
    volatile uint8_t *udp_tx_regs, volatile uint8_t *udp_rx_regs,
    uint8_t payload[], uint32_t payload_size);

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

int main(int argc, char **argv) {
    int fd;

    off_t regs_offset, udp_tx_offset, udp_rx_offset;
    size_t regs_size, udp_tx_regs_size, udp_rx_regs_size;

    volatile uint8_t *regs;
    volatile uint8_t *udp_tx_regs, *udp_rx_regs;
    void *udp_tx_buffer, *udp_rx_buffer;

    size_t udp_tx_size = 16 * 1024;
    size_t udp_rx_size = 16 * 1024;

    uint32_t payload_size = 256;

    fd = open("/dev/accnet-misc", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (do_ioctl(fd, 0, &regs_offset, &regs_size) != 0 ||
    do_ioctl(fd, 1, &udp_rx_offset, &udp_tx_regs_size) != 0 ||
    do_ioctl(fd, 2, &udp_tx_offset, &udp_rx_regs_size) != 0) 
    {
        close(fd);
        return -1;
    }

    /* mmap registers and dma memory */
    regs = (volatile uint8_t *)mmap(NULL, regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(0));
    if (regs == MAP_FAILED)
    {
        perror("mmap regs failed");
        close(fd);
        return -1;
    }

    udp_tx_regs = (volatile uint8_t *)mmap(NULL, udp_tx_regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(1));
    if (udp_tx_regs == MAP_FAILED)
    {
        perror("udp_tx_regs mmap regs failed");
        close(fd);
        return -1;
    }
    
    udp_rx_regs = (volatile uint8_t *)mmap(NULL, udp_rx_regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(2));
    if (udp_rx_regs == MAP_FAILED)
    {
        perror("udp_rx_regs mmap regs failed");
        close(fd);
        return -1;
    }

    udp_tx_buffer = mmap(NULL, udp_tx_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(3));
    if (udp_tx_buffer == MAP_FAILED) {
        perror("mmap udp tx");
        close(fd);
        return -1;
    }

    udp_rx_buffer = mmap(NULL, udp_rx_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(4));
    if (udp_rx_buffer == MAP_FAILED) {
        perror("mmap udp rx");
        close(fd);
        return -1;
    }

    /* Initialize udp on nic registers */
    // accnet_reg_write32(regs, 0x000140, 0x35064de2 & 0xffffffff);  // 0c:42:a1:a8:2d:e6

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    /* Run Test */
    test_nic_udp(udp_tx_buffer, udp_rx_buffer, regs, udp_tx_regs, udp_rx_regs,
        payload, payload_size); 

    /* Cleanup */
    munmap((void *) regs, regs_size);
    munmap((void *) udp_tx_regs, udp_tx_regs_size);
    munmap((void *) udp_rx_regs, udp_rx_regs_size);
    munmap((void *) udp_rx_buffer, udp_rx_size);
    munmap((void *) udp_tx_buffer, udp_tx_size);
    close(fd);

    return 0;
}

void test_nic_udp(void* udp_tx_buffer, void* udp_rx_buffer, volatile uint8_t *regs, 
    volatile uint8_t *udp_tx_regs, volatile uint8_t *udp_rx_regs,
    uint8_t payload[], uint32_t payload_size)
{
    // uint32_t rx_head, rx_tail;
    // uint32_t head, tail;

    // // Read buffer head/tail
    // head = accnet_reg_read32(regs, 0x000128);
    // tail = accnet_reg_read32(regs, 0x00012c);

}
