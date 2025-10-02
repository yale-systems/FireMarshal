#ifndef __IOCACHE_H
#define __IOCACHE_H

#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/circ_buf.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/eventfd.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include <linux/dma-mapping.h>
#include <linux/miscdevice.h> 

#include <linux/u64_stats_sync.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/io.h>   /* iowriteXX */
#define REG(base, off) ((void __iomem *)((u8 __iomem *)(base) + (off)))

#define IOCACHE_NAME "iocache"

#define NUM_CPUS 	4

#define IOCACHE_CACHE_ENTRY_COUNT		64
#define IOCACHE_UDP_RING_SIZE 			32 * 1024 		// 8KB

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
#define IOCACHE_REG_PROC_CPU(row)         IOCACHE_REG((row), IOCACHE_PROC_CPU_OFF)
#define IOCACHE_REG_PROC_PTR(row)         IOCACHE_REG((row), IOCACHE_PROC_PTR_OFF)
#define IOCACHE_REG_RX_RING_ADDR(row)     IOCACHE_REG((row), IOCACHE_RX_RING_ADDR_OFF)
#define IOCACHE_REG_RX_RING_SIZE(row)     IOCACHE_REG((row), IOCACHE_RX_RING_SIZE_OFF)
#define IOCACHE_REG_TX_RING_ADDR(row)     IOCACHE_REG((row), IOCACHE_TX_RING_ADDR_OFF)
#define IOCACHE_REG_TX_RING_SIZE(row)     IOCACHE_REG((row), IOCACHE_TX_RING_SIZE_OFF)


#define IOCACHE_INTMASK_RX 			1
#define IOCACHE_INTMASK_TXCOMP 		2
#define IOCACHE_INTMASK_BOTH 		3

#define ETH_HEADER_BYTES 14
#define ALIGN_BYTES 64
#define ALIGN_MASK 0x3f
#define ALIGN_SHIFT 6
#define MAX_FRAME_SIZE (CONFIG_ACCNET_MTU + ETH_HEADER_BYTES + NET_IP_ALIGN)
#define DMA_PTR_ALIGN(p) ((typeof(p)) (__ALIGN_KERNEL((uintptr_t) (p), ALIGN_BYTES)))
#define DMA_LEN_ALIGN(n) (((((n) - 1) >> ALIGN_SHIFT) + 1) << ALIGN_SHIFT)
#define MACADDR_BYTES 6

#define MAGIC_CHAR 0xCCCCCCCCUL

struct iocache_device {
	struct device *dev;
	
	int rx_irq[NUM_CPUS], rx_hwirq[NUM_CPUS];
	int txcomp_irq[NUM_CPUS], txcomp_hwirq[NUM_CPUS];

	resource_size_t hw_regs_control_size;
	phys_addr_t hw_regs_control_phys;
	void __iomem *iomem;

    struct miscdevice misc_dev; // Add the miscdevice member here	

	struct eventfd_ctx *ev_ctx;  /* signaled from IRQ */
    spinlock_t          ev_lock; /* protects ev_ctx */

    spinlock_t          ring_alloc_lock;
    spinlock_t          rxkick_lock;
	
	unsigned long magic;

	void __iomem *plic_base;

	// DMA buffer UDP TX
	size_t 		dma_region_len_udp_tx[IOCACHE_CACHE_ENTRY_COUNT];
	void	   *dma_region_udp_tx[IOCACHE_CACHE_ENTRY_COUNT];
	void	   *dma_region_udp_tx_aligned[IOCACHE_CACHE_ENTRY_COUNT];
	dma_addr_t 	dma_region_addr_udp_tx[IOCACHE_CACHE_ENTRY_COUNT];
	dma_addr_t 	dma_region_addr_udp_tx_aligned[IOCACHE_CACHE_ENTRY_COUNT];

	// DMA buffer UDP RX
	size_t 		dma_region_len_udp_rx[IOCACHE_CACHE_ENTRY_COUNT];
	void 	   *dma_region_udp_rx[IOCACHE_CACHE_ENTRY_COUNT];
	void 	   *dma_region_udp_rx_aligned[IOCACHE_CACHE_ENTRY_COUNT];
	dma_addr_t 	dma_region_addr_udp_rx[IOCACHE_CACHE_ENTRY_COUNT];
	dma_addr_t 	dma_region_addr_udp_rx_aligned[IOCACHE_CACHE_ENTRY_COUNT];

	struct u64_stats_sync syncp;
    u64 isr_ktime, entry_ktime, claim_ktime, syscall_time;

	wait_queue_head_t wq;
    // atomic_t ready; 

	struct completion ready_comp;

	struct hrtimer to_hrtimer;
    ktime_t to_period;    // e.g., KTIME_MS(1)
};

static int iocache_misc_open(struct inode *inode, struct file *filp);
static int iocache_misc_mmap(struct file *filp, struct vm_area_struct *vma);
static int iocache_misc_release(struct inode *inode, struct file *filp);
static long iocache_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* __IOCACHE_H */