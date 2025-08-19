#ifndef __ACCNET_LIB_H
#define __ACCNET_LIB_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#define accnet_reg_read64(base, reg) (((volatile uint64_t *)(base))[(reg)/8])
#define accnet_reg_write64(base, reg, val) (((volatile uint64_t *)(base))[(reg)/8]) = val

#define accnet_reg_read32(base, reg) (((volatile uint32_t *)(base))[(reg)/4])
#define accnet_reg_write32(base, reg, val) (((volatile uint32_t *)(base))[(reg)/4]) = val

#define accnet_reg_read16(base, reg) (((volatile uint16_t *)(base))[(reg)/2])
#define accnet_reg_write16(base, reg, val) (((volatile uint16_t *)(base))[(reg)/2]) = val

#define accnet_reg_read8(base, reg) (((volatile uint8_t *)(base))[(reg)/1])
#define accnet_reg_write8(base, reg, val) (((volatile uint8_t *)(base))[(reg)/1]) = val

#define MAP_INDEX(idx) (((uint64_t) idx) << 40)

// UDP RX Engine registers
#define ACCNET_UDP_RX_RING_BASE            0x00
#define ACCNET_UDP_RX_RING_SIZE            0x08
#define ACCNET_UDP_RX_RING_HEAD            0x0C
#define ACCNET_UDP_RX_RING_TAIL            0x10

// UDP TX Engine registers
#define ACCNET_UDP_TX_RING_BASE            0x00
#define ACCNET_UDP_TX_RING_SIZE            0x08
#define ACCNET_UDP_TX_RING_HEAD            0x0C
#define ACCNET_UDP_TX_RING_TAIL            0x10
#define ACCNET_UDP_TX_MTU                  0x14
#define ACCNET_UDP_TX_HDR_MAC_SRC          0x20
#define ACCNET_UDP_TX_HDR_MAC_DST          0x28
#define ACCNET_UDP_TX_HDR_IP_SRC           0x30
#define ACCNET_UDP_TX_HDR_IP_DST           0x34
#define ACCNET_UDP_TX_HDR_IP_TOS           0x38
#define ACCNET_UDP_TX_HDR_IP_TTL           0x39
#define ACCNET_UDP_TX_HDR_IP_ID            0x3A
#define ACCNET_UDP_TX_HDR_UDP_SRC_PORT     0x40
#define ACCNET_UDP_TX_HDR_UDP_DST_PORT     0x42
#define ACCNET_UDP_TX_HDR_UDP_CSUM         0x44

// Control registers
#define ACCNET_CTRL_INTR_MASK              0x00
#define ACCNET_CTRL_FILTER_PORT            0x08
#define ACCNET_CTRL_FILTER_IP              0x10

struct accnet_info {
    bool is_open;

    int fd;
    size_t ALIGN;
    
    off_t regs_offset, udp_tx_offset, udp_rx_offset;
    size_t regs_size, udp_tx_regs_size, udp_rx_regs_size;

    volatile uint8_t *regs;
    volatile uint8_t *udp_tx_regs, *udp_rx_regs;
    void *udp_tx_buffer, *udp_rx_buffer;
    void *udp_tx_buffer_aligned, *udp_rx_buffer_aligned;

    size_t udp_tx_size;
    size_t udp_rx_size;
};

int accnet_open(char *file, struct accnet_info *accnet, bool do_init);
int accnet_close(struct accnet_info *accnet);

#endif /* ACCNET_LIB_H */