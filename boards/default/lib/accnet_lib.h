#ifndef __ACCNET_LIB_H
#define __ACCNET_LIB_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "iocache_lib.h"
#include "common.h"

/* =========================  UDP  =========================== */
#define ACCNET_UDP_RING_SIZE 	256 * 1024 		// 64KB
#define ACCNET_UDP_RING_COUNT	1

/* ===================================================================== */
/* =========================  UDP RX ENGINE  =========================== */
/* ===================================================================== */
#define ACCNET_UDP_RX_RING_STRIDE          0x20UL

#define ACCNET_UDP_RX_RING_HEAD_OFF             0x00UL
#define ACCNET_UDP_RX_RING_TAIL_OFF             0x04UL
#define ACCNET_UDP_RX_RING_DROP_OFF             0x08UL
#define ACCNET_UDP_RX_RING_LAST_TIMESTAMP_OFF  	0x10UL

#define ACCNET_UDP_RX_RING_REG(r, off)    ((uint64_t)(r) * ACCNET_UDP_RX_RING_STRIDE + (off))

/* Per-ring convenience */
#define ACCNET_UDP_RX_RING_HEAD(r)              ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_HEAD_OFF)   /* 32-bit */
#define ACCNET_UDP_RX_RING_TAIL(r)              ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_TAIL_OFF)   /* 32-bit */
#define ACCNET_UDP_RX_RING_DROP(r)              ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_DROP_OFF)   /* 32-bit, RO */
#define ACCNET_UDP_RX_RING_LAST_TIMESTAMP(r)    ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_LAST_TIMESTAMP_OFF)   /* 64-bit, RO */

/* Engine-level IRQ */
#define ACCNET_UDP_RX_IRQ_PENDING         0x400UL
#define ACCNET_UDP_RX_IRQ_CLEAR           0x404UL

/* ===================================================================== */
/* =========================  UDP TX ENGINE  =========================== */
/* ===================================================================== */
#define ACCNET_UDP_TX_RING_STRIDE          0x10UL

#define ACCNET_UDP_TX_RING_HEAD_OFF             0x00UL
#define ACCNET_UDP_TX_RING_TAIL_OFF             0x04UL
#define ACCNET_UDP_TX_RING_LAST_TIMESTAMP_OFF   0x08UL

#define ACCNET_UDP_TX_RING_REG(r, off)    ((uint64_t)(r) * ACCNET_UDP_TX_RING_STRIDE + (off))

/* Per-ring convenience */
#define ACCNET_UDP_TX_RING_HEAD(r)              ACCNET_UDP_TX_RING_REG((r), ACCNET_UDP_TX_RING_HEAD_OFF)   /* 32-bit */
#define ACCNET_UDP_TX_RING_TAIL(r)              ACCNET_UDP_TX_RING_REG((r), ACCNET_UDP_TX_RING_TAIL_OFF)   /* 32-bit */
#define ACCNET_UDP_TX_RING_LAST_TIMESTAMP(r)    ACCNET_UDP_TX_RING_REG((r), ACCNET_UDP_TX_RING_LAST_TIMESTAMP_OFF)   /* 64-bit, RO */

/* Global/header regs */
#define ACCNET_UDP_TX_MTU                 0x400UL  /* 16-bit */
#define ACCNET_UDP_TX_HDR_MAC_DST         0x408UL  /* 64-bit; write low 48b used */
#define ACCNET_UDP_TX_HDR_MAC_SRC         0x410UL  /* 64-bit; write low 48b used */
#define ACCNET_UDP_TX_HDR_IP_TOS          0x420UL  /* 8-bit  */
#define ACCNET_UDP_TX_HDR_IP_TTL          0x424UL  /* 8-bit  */
#define ACCNET_UDP_TX_HDR_IP_ID           0x428UL  /* 16-bit */
#define ACCNET_UDP_TX_HDR_UDP_CSUM0_OK    0x42CUL  /* 1-bit  */

/* TX IRQ */
#define ACCNET_UDP_TX_IRQ_PENDING         0x430UL
#define ACCNET_UDP_TX_IRQ_CLEAR           0x434UL

// Control registers
#define ACCNET_CTRL_INTR_MASK              0x00
#define ACCNET_CTRL_TIMESTAMP              0x10

struct ring_info {
    uint32_t rx_head, rx_tail, rx_size;
    uint32_t tx_head, tx_tail, tx_size;
};


struct accnet_info {
    int fd;
    size_t ALIGN;
    
    off_t regs_offset, udp_tx_offset, udp_rx_offset;
    size_t regs_size, udp_tx_regs_size, udp_rx_regs_size;

    volatile uint8_t *regs;
    volatile uint8_t *udp_tx_regs, *udp_rx_regs;

    struct iocache_info *iocache;

    struct ring_info ring;
};

int accnet_open(char *file, struct accnet_info *accnet, struct iocache_info *iocache, bool do_init);
int accnet_close(struct accnet_info *accnet);

int accnet_start_ring(struct accnet_info *accnet);
int accnet_setup_connection(struct accnet_info *accnet, struct connection_info *connection);

uint64_t accnet_get_outside_ticks(struct accnet_info *accnet);

size_t accnet_send(struct accnet_info *accnet, void *buffer, size_t len);

static inline uint64_t accnet_get_time(struct accnet_info *accnet) 
{
    reg_read64(accnet->regs, ACCNET_CTRL_TIMESTAMP);
}

static inline void accnet_set_rx_head(struct accnet_info *accnet, uint32_t val) 
{
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_RX_RING_HEAD(accnet->iocache->row), val);
}
static inline void accnet_set_tx_tail(struct accnet_info *accnet, uint32_t val) 
{
    reg_write32(accnet->udp_rx_regs, ACCNET_UDP_TX_RING_TAIL(accnet->iocache->row), val);
}
static inline uint32_t accnet_get_rx_head(struct accnet_info *accnet) 
{
    return reg_read32(accnet, ACCNET_UDP_RX_RING_HEAD(accnet->iocache->row));
}
static inline uint32_t accnet_get_rx_tail(struct accnet_info *accnet) 
{
    return reg_read32(accnet, ACCNET_UDP_RX_RING_TAIL(accnet->iocache->row));
}
static inline uint32_t accnet_get_tx_head(struct accnet_info *accnet) 
{
    return reg_read32(accnet, ACCNET_UDP_TX_RING_HEAD(accnet->iocache->row));
}
static inline uint32_t accnet_get_tx_tail(struct accnet_info *accnet) 
{
    return reg_read32(accnet, ACCNET_UDP_TX_RING_TAIL(accnet->iocache->row));
}

#endif /* ACCNET_LIB_H */