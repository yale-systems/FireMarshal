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

#define IOCACHE_INTMASK_RX 			1
#define IOCACHE_INTMASK_TXCOMP 		2
#define IOCACHE_INTMASK_BOTH 		3

#define MAGIC_CHAR 0xCCCCCCCCUL

struct iocache_device {
	struct device *dev;
	
	int rx_irq, rx_hwirq;
	int txcomp_irq, txcomp_hwirq;

	resource_size_t hw_regs_control_size;
	phys_addr_t hw_regs_control_phys;
	void __iomem *iomem;

    struct miscdevice misc_dev; // Add the miscdevice member here	

	struct eventfd_ctx *ev_ctx;  /* signaled from IRQ */
    spinlock_t          ev_lock; /* protects ev_ctx */
	
	unsigned long magic;

	void __iomem *plic_base;

	struct u64_stats_sync syncp;
    u64 isr_ktime, entry_ktime, claim_ktime;

	wait_queue_head_t wq;
    atomic_t ready; 
};

static int iocache_misc_open(struct inode *inode, struct file *filp);
static int iocache_misc_mmap(struct file *filp, struct vm_area_struct *vma);
static int iocache_misc_release(struct inode *inode, struct file *filp);
static long iocache_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* __IOCACHE_H */