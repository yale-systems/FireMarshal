#ifndef __ACCNET_LIB_H
#define __ACCNET_LIB_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "common.h"

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

static inline void accnet_setup_connection(struct accnet_info *accnet, struct connection_info *connection) {
    if (!accnet) 
        return;
    reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_SRC,       connection->src_ip);
	reg_write32(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_IP_DST,       connection->dst_ip);
    reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_SRC_PORT, connection->src_port);
	reg_write16(accnet->udp_tx_regs, ACCNET_UDP_TX_HDR_UDP_DST_PORT, connection->dst_port);
}

#endif /* ACCNET_LIB_H */