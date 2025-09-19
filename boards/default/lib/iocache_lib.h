#ifndef __IOCACHE_LIB_H
#define __IOCACHE_LIB_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "common.h"

#define NUM_CPUS 	NR_CPUS

#define IOCACHE_CACHE_ENTRY_COUNT		64

/* ---- Sub-block bases (must match Scala) ---- */
#define IOCACHE_INT_BASE     0x000UL
#define IOCACHE_ALLOC_BASE   0x080UL
#define IOCACHE_KICK_BASE    0x100UL
#define IOCACHE_SCHED_BASE   0x200UL
#define IOCACHE_TABLE_BASE   0x300UL   /* table starts at +0x300 */

/* ---- Bus parameters ---- */
#define IOCACHE_BEAT_BYTES   8UL
#define IOCACHE_ROW_STRIDE   0x100UL  /* 256B = 32 beats */

/* ==== Per-CPU Interrupt Masks ====
 * For CPU c at INT_BASE + 0x10*c:
 *   +0x00: int_mask_rx[c]     (1 bit)
 *   +0x08: int_mask_txcomp[c] (1 bit)
 */
#define IOCACHE_REG_INTMASK_RX(c)     	 (IOCACHE_INT_BASE  + (0x10UL*(c)) + 0x00UL)
#define IOCACHE_REG_INTMASK_TXCOMP(c)    (IOCACHE_INT_BASE  + (0x10UL*(c)) + 0x08UL)

/* ==== Per-CPU Scheduler ====
 * For CPU c at SCHED_BASE + 0x10*c:
 *   +0x00: SCHED_READ[c]  (64-bit, advances RR)
 *   +0x08: SCHED_PEEK[c]  (64-bit, no advance)
 */
#define IOCACHE_REG_SCHED_READ(c)      (IOCACHE_SCHED_BASE + (0x10UL*(c)) + 0x00UL)
#define IOCACHE_REG_SCHED_PEEK(c)      (IOCACHE_SCHED_BASE + (0x10UL*(c)) + 0x08UL)

/* ==== RX Kick-All (by CPU) ==== */
#define IOCACHE_REG_RX_KICK_ALL_CPU     (IOCACHE_KICK_BASE + 0x00UL) /* WO 32b */
#define IOCACHE_REG_RX_KICK_ALL_COUNT   (IOCACHE_KICK_BASE + 0x08UL) /* RO */
#define IOCACHE_REG_RX_KICK_ALL_MASK    (IOCACHE_KICK_BASE + 0x10UL) /* RO */

/* ==== Allocator (first empty row) ==== 
 * +0x00: FIRST_EMPTY_ROW (RO, 32b). Returns: 0 if none.
 */
#define IOCACHE_REG_ALLOC_FIRST_EMPTY   (IOCACHE_ALLOC_BASE + 0x00UL)

/* ==== Row field offsets (relative to row base) ==== 
 * Beat i offset = i * IOCACHE_BEAT_BYTES
 */
#define IOCACHE_ENABLED_OFF           0x00UL  /* beat 0 */
#define IOCACHE_PROTOCOL_OFF          0x08UL  /* beat 1 */
#define IOCACHE_SRC_IP_OFF            0x10UL  /* beat 2 */
#define IOCACHE_SRC_PORT_OFF          0x18UL  /* beat 3 */
#define IOCACHE_DST_IP_OFF            0x20UL  /* beat 4 */
#define IOCACHE_DST_PORT_OFF          0x28UL  /* beat 5 */
#define IOCACHE_RX_AVAILABLE_OFF      0x30UL  /* beat 6 */
#define IOCACHE_RX_SUSPENDED_OFF      0x38UL  /* beat 7 */
#define IOCACHE_TXCOMP_AVAILABLE_OFF  0x40UL  /* beat 8 */
#define IOCACHE_TXCOMP_SUSPENDED_OFF  0x48UL  /* beat 9 */
#define IOCACHE_FLAGS_RO_OFF          0x50UL  /* beat 10 */
#define IOCACHE_CONN_ID_OFF           0x58UL  /* beat 11 */
#define IOCACHE_PROC_PTR_OFF          0x60UL  /* beat 12 */
#define IOCACHE_PROC_CPU_OFF          0x68UL  /* beat 13 */
#define IOCACHE_RX_RING_ADDR_OFF      0x70UL  /* beat 14 */
#define IOCACHE_RX_RING_SIZE_OFF      0x78UL  /* beat 15 */
#define IOCACHE_TX_RING_ADDR_OFF      0x80UL  /* beat 14 */
#define IOCACHE_TX_RING_SIZE_OFF      0x88UL  /* beat 15 */

/* Row address helper */
#define IOCACHE_REG(row, off) \
    (IOCACHE_TABLE_BASE + (row) * IOCACHE_ROW_STRIDE + (off))

/* Convenience macros */
#define IOCACHE_REG_ENABLED(row)          IOCACHE_REG((row), IOCACHE_ENABLED_OFF)
#define IOCACHE_REG_PROTOCOL(row)         IOCACHE_REG((row), IOCACHE_PROTOCOL_OFF)
#define IOCACHE_REG_SRC_IP(row)           IOCACHE_REG((row), IOCACHE_SRC_IP_OFF)
#define IOCACHE_REG_SRC_PORT(row)         IOCACHE_REG((row), IOCACHE_SRC_PORT_OFF)
#define IOCACHE_REG_DST_IP(row)           IOCACHE_REG((row), IOCACHE_DST_IP_OFF)
#define IOCACHE_REG_DST_PORT(row)         IOCACHE_REG((row), IOCACHE_DST_PORT_OFF)
#define IOCACHE_REG_RX_AVAILABLE(row)     IOCACHE_REG((row), IOCACHE_RX_AVAILABLE_OFF)
#define IOCACHE_REG_RX_SUSPENDED(row)     IOCACHE_REG((row), IOCACHE_RX_SUSPENDED_OFF)
#define IOCACHE_REG_TXCOMP_AVAILABLE(row) IOCACHE_REG((row), IOCACHE_TXCOMP_AVAILABLE_OFF)
#define IOCACHE_REG_TXCOMP_SUSPENDED(row) IOCACHE_REG((row), IOCACHE_TXCOMP_SUSPENDED_OFF)
#define IOCACHE_REG_FLAGS_RO(row)         IOCACHE_REG((row), IOCACHE_FLAGS_RO_OFF)
#define IOCACHE_REG_CONN_ID(row)          IOCACHE_REG((row), IOCACHE_CONN_ID_OFF)
// #define IOCACHE_REG_PROC_PTR(row)         IOCACHE_REG((row), IOCACHE_PROC_PTR_OFF)
// #define IOCACHE_REG_PROC_CPU(row)         IOCACHE_REG((row), IOCACHE_PROC_CPU_OFF)
#define IOCACHE_REG_RX_RING_ADDR(row)     IOCACHE_REG((row), IOCACHE_RX_RING_ADDR_OFF)
#define IOCACHE_REG_RX_RING_SIZE(row)     IOCACHE_REG((row), IOCACHE_RX_RING_SIZE_OFF)
#define IOCACHE_REG_TX_RING_ADDR(row)     IOCACHE_REG((row), IOCACHE_TX_RING_ADDR_OFF)
#define IOCACHE_REG_TX_RING_SIZE(row)     IOCACHE_REG((row), IOCACHE_TX_RING_SIZE_OFF)


struct iocache_info {
    int fd;
    size_t ALIGN;
    
    off_t regs_offset;
    size_t regs_size;

    int row;
    int cpu;

    void *udp_tx_buffer, *udp_rx_buffer;
    void *udp_tx_buffer_aligned, *udp_rx_buffer_aligned;

    size_t udp_tx_size;
    size_t udp_rx_size;

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

int iocache_start_scheduler(struct iocache_info *iocache);
int iocache_stop_scheduler(struct iocache_info *iocache);

static void _iocache_setup_connection(struct iocache_info *iocache, struct connection_info *entry, int row) {
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    entry->protocol);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      entry->src_ip);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    entry->src_port);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      entry->dst_ip);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    entry->dst_port);
    reg_write16(iocache->regs, IOCACHE_REG_CONN_ID(row),     entry->src_port);
    
    reg_write8 (iocache->regs, IOCACHE_REG_ENABLED(row),     1);
    mmio_wmb();
}

static void iocache_setup_connection(struct iocache_info *iocache, struct connection_info *entry) {
    _iocache_setup_connection(iocache, entry, iocache->row);
}

static void iocache_clear_connection(struct iocache_info *iocache) {
    int row = iocache->row;
    reg_write8 (iocache->regs, IOCACHE_REG_PROTOCOL(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_SRC_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_SRC_PORT(row),    0);
    reg_write32(iocache->regs, IOCACHE_REG_DST_IP(row),      0);
    reg_write16(iocache->regs, IOCACHE_REG_DST_PORT(row),    0);
    mmio_wmb();
}

static inline bool iocache_is_rx_available(struct iocache_info *iocache) {
    return reg_read8(iocache->regs, IOCACHE_REG_RX_AVAILABLE(iocache->row));
}

static inline bool iocache_is_txcomp_available(struct iocache_info *iocache) {
    return reg_read8(iocache->regs, IOCACHE_REG_TXCOMP_AVAILABLE(iocache->row));
}

static inline void iocache_set_tx_ring_addr(struct iocache_info *iocache, uint64_t val, int row) 
{
    reg_write64(iocache->regs, IOCACHE_REG_TX_RING_ADDR(row), val);
}

static inline void iocache_set_tx_ring_size(struct iocache_info *iocache, uint32_t val, int row) 
{
    reg_write32(iocache->regs, IOCACHE_REG_TX_RING_SIZE(row), val);
}

static inline void iocache_set_rx_ring_addr(struct iocache_info *iocache, uint64_t val, int row) 
{
    reg_write64(iocache->regs, IOCACHE_REG_RX_RING_ADDR(row), val);
}

static inline void iocache_set_rx_ring_size(struct iocache_info *iocache, uint32_t val, int row) 
{
    reg_write32(iocache->regs, IOCACHE_REG_RX_RING_SIZE(row), val);
}

#endif      // __IOCACHE_LIB_H