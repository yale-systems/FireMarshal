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

#define DMA_ALIGN(x) (((x) + 63) & ~63)

#define UDP_TEST_LEN 2048
#define UDP_RING_SIZE 4096
#define UDP_ARRAY_LEN DMA_ALIGN(UDP_RING_SIZE)

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
    void *udp_tx_buffer_aligned, *udp_rx_buffer_aligned;
    uintptr_t p;

    size_t udp_tx_size = 8 * 1024;
    size_t udp_rx_size = 8 * 1024;

    const size_t ALIGN = 64;

    uint32_t payload_size = 1024;

    fd = open("/dev/accnet-misc", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (do_ioctl(fd, 0, &regs_offset, &regs_size) != 0 ||
    do_ioctl(fd, 1, &udp_tx_offset, &udp_tx_regs_size) != 0 ||
    do_ioctl(fd, 2, &udp_rx_offset, &udp_rx_regs_size) != 0) 
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

    udp_tx_buffer = mmap(NULL, udp_tx_size + (ALIGN - 1), PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(3));
    if (udp_tx_buffer == MAP_FAILED) {
        perror("mmap udp tx");
        close(fd);
        return -1;
    }
    p = (uintptr_t)udp_tx_buffer;
    udp_tx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    udp_rx_buffer = mmap(NULL, udp_rx_size + (ALIGN - 1), PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_INDEX(4));
    if (udp_rx_buffer == MAP_FAILED) {
        perror("mmap udp rx");
        close(fd);
        return -1;
    }
    p = (uintptr_t)udp_rx_buffer;
    udp_rx_buffer_aligned = (void *)((p + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1));

    /* Initialize udp on nic registers */
    // Contor registers
	accnet_reg_write32(regs, ACCNET_CTRL_FILTER_PORT, 1234);
	accnet_reg_write32(regs, ACCNET_CTRL_FILTER_IP,   0xA000001); // 10.0.0.1
	// RX
	accnet_reg_write32(udp_rx_regs, ACCNET_UDP_RX_RING_SIZE, UDP_RING_SIZE);
	// accnet_reg_write32(udp_rx_regs, ACCNET_UDP_RX_RING_HEAD, 0);
	// accnet_reg_write32(udp_rx_regs, ACCNET_UDP_RX_RING_TAIL, 0);
	// TX
	accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_RING_SIZE, UDP_RING_SIZE);
	// accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_RING_HEAD, 0);
	// accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, 0);
	accnet_reg_write16(udp_tx_regs, ACCNET_UDP_TX_MTU, 1472);
	accnet_reg_write64(udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_SRC, 0x112233445566); 
	accnet_reg_write64(udp_tx_regs, ACCNET_UDP_TX_HDR_MAC_DST, 0x0c42a1a82de6);// 0c:42:a1:a8:2d:e6
	accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_HDR_IP_SRC, 0x0a0b0c0d);
	accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_HDR_IP_DST, 0x0A000001);
	accnet_reg_write8 (udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TOS, 0);
	accnet_reg_write8 (udp_tx_regs, ACCNET_UDP_TX_HDR_IP_TTL, 64);
	accnet_reg_write16(udp_tx_regs, ACCNET_UDP_TX_HDR_IP_ID, 0);
	accnet_reg_write16(udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_SRC_PORT, 1111);
	accnet_reg_write16(udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_DST_PORT, 1234);
	// accnet_reg_write8(udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_CSUM, 1500);

    /* Initializing payload */
    uint8_t payload[payload_size];
    for (uint32_t i = 0; i < payload_size; i++) {
        payload[i] = i & 0xff;
    }

    /* Run Test */
    test_nic_udp(udp_tx_buffer_aligned, udp_rx_buffer_aligned, regs, udp_tx_regs, udp_rx_regs,
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
    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;

    // Read buffer head/tail
    rx_head = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_tail = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    rx_size = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);

    tx_head = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);

    printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);
    printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);

    printf("Copying mem...\n");
    memcpy(udp_tx_buffer, payload, payload_size);

    uint32_t val = (tx_tail + payload_size) % UDP_RING_SIZE;
    printf("Updating tx tail to %u...\n", val);
    accnet_reg_write32(udp_tx_regs, ACCNET_UDP_TX_RING_TAIL, val);

    printf("sleeping for 1 second\n");
    sleep(1);

    // Read buffer head/tail
    tx_head = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_HEAD);
    tx_tail = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_TAIL);
    tx_size = accnet_reg_read32(udp_tx_regs, ACCNET_UDP_TX_RING_SIZE);
    printf("TX_HEAD: %u, TX_TAIL: %u, TX_SIZE: %u\n", tx_head, tx_tail, tx_size);

    rx_head = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_HEAD);
    rx_tail = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_TAIL);
    rx_size = accnet_reg_read32(udp_rx_regs, ACCNET_UDP_RX_RING_SIZE);
    printf("RX_HEAD: %u, RX_TAIL: %u, RX_SIZE: %u\n", rx_head, rx_tail, rx_size);


    printf("Original Payload:\n");
    for (uint32_t i = 0; i < payload_size; i++) {
        printf("%02x", payload[i]);
    }
    printf("\n");
    printf("RX Payload:\n");
    for (uint32_t i = 0; i < payload_size; i++) {
        printf("%02x", ((uint8_t*)udp_rx_buffer)[i]);
    }
    printf("\n");

}
