#ifndef __IOCACHE_LIB_H
#define __IOCACHE_LIB_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "common.h"

/* ---- Bus parameters ---- */
#define IOCACHE_BEAT_BYTES   8UL     /* one TileLink beat = 8 bytes = 64 bits */
#define IOCACHE_ROW_STRIDE   0x80UL  /* 128B stride per row (16 * BEAT_BYTES) */

/* ---- Row field offsets (relative to row base) ---- */
#define IOCACHE_ENABLED_OFF           0x00UL
#define IOCACHE_PROTOCOL_OFF          0x08UL
#define IOCACHE_SRC_IP_OFF            0x10UL
#define IOCACHE_SRC_PORT_OFF          0x18UL
#define IOCACHE_DST_IP_OFF            0x20UL
#define IOCACHE_DST_PORT_OFF          0x28UL
#define IOCACHE_RX_AVAILABLE_OFF      0x30UL
#define IOCACHE_RX_SUSPENDED_OFF      0x38UL
#define IOCACHE_TXCOMP_AVAILABLE_OFF  0x40UL
#define IOCACHE_TXCOMP_SUSPENDED_OFF  0x48UL
#define IOCACHE_FLAGS_OFF             0x50UL

/* ---- Macro to compute register address for row i ---- */
#define IOCACHE_TABLE_BASE   0x100UL   /* table starts at +0x100 */
#define IOCACHE_REG(row, off) \
    (IOCACHE_TABLE_BASE + (row) * IOCACHE_ROW_STRIDE + (off))

/* ---- Global registers ---- */
#define IOCACHE_REG_INTMASK_RX  		0x00UL
#define IOCACHE_REG_INTMASK_TXCOMP  	0x08UL

/* ---- Convenience macros ---- */
#define IOCACHE_REG_ENABLED(row)            IOCACHE_REG((row), IOCACHE_ENABLED_OFF)
#define IOCACHE_REG_PROTOCOL(row)           IOCACHE_REG((row), IOCACHE_PROTOCOL_OFF)
#define IOCACHE_REG_SRC_IP(row)             IOCACHE_REG((row), IOCACHE_SRC_IP_OFF)
#define IOCACHE_REG_SRC_PORT(row)           IOCACHE_REG((row), IOCACHE_SRC_PORT_OFF)
#define IOCACHE_REG_DST_IP(row)             IOCACHE_REG((row), IOCACHE_DST_IP_OFF)
#define IOCACHE_REG_DST_PORT(row)           IOCACHE_REG((row), IOCACHE_DST_PORT_OFF)
#define IOCACHE_REG_RX_AVAILABLE(row)       IOCACHE_REG((row), IOCACHE_RX_AVAILABLE_OFF)
#define IOCACHE_REG_RX_SUSPENDED(row)       IOCACHE_REG((row), IOCACHE_RX_SUSPENDED_OFF)
#define IOCACHE_REG_TXCOMP_AVAILABLE(row)   IOCACHE_REG((row), IOCACHE_TXCOMP_AVAILABLE_OFF)
#define IOCACHE_REG_TXCOMP_SUSPENDED(row)   IOCACHE_REG((row), IOCACHE_TXCOMP_SUSPENDED_OFF)
#define IOCACHE_REG_FLAGS(row)         		IOCACHE_REG((row), IOCACHE_FLAGS_OFF)


struct iocache_info {
    int fd;
    size_t ALIGN;
    
    off_t regs_offset;
    size_t regs_size;

    volatile uint8_t *regs;

    int efd;
    int ep;
};

int iocache_open(char *file, struct iocache_info *iocache);
int iocache_close(struct iocache_info *iocache);
int iocache_wait_on_rx(struct iocache_info *iocache);
int iocache_wait_on_txcomp(struct iocache_info *iocache);
int iocache_get_last_irq_ns(struct iocache_info *iocache, __u64 *ns);
int iocache_get_last_ktimes(struct iocache_info *iocache, __u64 ktimes[3]);
int iocache_print_proc_util(struct iocache_info *iocache);

static inline void iocache_setup_connection(struct iocache_info *iocache, struct connection_info *entry) {
    int row = 0;
    reg_write8 (iocache->regs, IOCACHE_REG_ENABLED(row),     1);
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    entry->protocol);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      entry->src_ip);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    entry->src_port);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      entry->dst_ip);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    entry->dst_port);
}

static inline void iocache_clear_connection(struct iocache_info *iocache) {
    int row = 0;
    reg_write8 (iocache->regs, IOCACHE_REG_ENABLED(row),     0);
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    0);
}

static inline bool iocache_is_rx_available(struct iocache_info *iocache) {
    int row = 0;
    return reg_read8(iocache->regs, IOCACHE_REG_RX_AVAILABLE(row));
}

static inline bool iocache_is_txcomp_available(struct iocache_info *iocache) {
    int row = 0;
    return reg_read8(iocache->regs, IOCACHE_REG_TXCOMP_AVAILABLE(row));
}

#endif      // __IOCACHE_LIB_H