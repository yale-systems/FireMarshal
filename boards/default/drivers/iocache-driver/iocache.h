#ifndef IOCACHE_H
#define IOCACHE_H

#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/circ_buf.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include <linux/dma-mapping.h>
#include <linux/miscdevice.h> 

#include <linux/io.h>   /* iowriteXX */

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
#define IOCACHE_RX_AWAKE_OFF          0x38UL
#define IOCACHE_TXCOMP_AVAILABLE_OFF  0x40UL
#define IOCACHE_TXCOMP_AWAKE_OFF      0x48UL
#define IOCACHE_FLAGS_OFF          0x50UL

/* ---- Macro to compute register address for row i ---- */
#define IOCACHE_TABLE_BASE   0x100UL   /* table starts at +0x100 */
#define IOCACHE_REG(row, off) \
    (IOCACHE_TABLE_BASE + (row) * IOCACHE_ROW_STRIDE + (off))

/* ---- Global registers ---- */
#define IOCACHE_REG_INTMASK  0x00UL

/* ---- Convenience macros ---- */
#define IOCACHE_REG_ENABLED(row)            IOCACHE_REG((row), IOCACHE_ENABLED_OFF)
#define IOCACHE_REG_PROTOCOL(row)           IOCACHE_REG((row), IOCACHE_PROTOCOL_OFF)
#define IOCACHE_REG_SRC_IP(row)             IOCACHE_REG((row), IOCACHE_SRC_IP_OFF)
#define IOCACHE_REG_SRC_PORT(row)           IOCACHE_REG((row), IOCACHE_SRC_PORT_OFF)
#define IOCACHE_REG_DST_IP(row)             IOCACHE_REG((row), IOCACHE_DST_IP_OFF)
#define IOCACHE_REG_DST_PORT(row)           IOCACHE_REG((row), IOCACHE_DST_PORT_OFF)
#define IOCACHE_REG_RX_AVAILABLE(row)       IOCACHE_REG((row), IOCACHE_RX_AVAILABLE_OFF)
#define IOCACHE_REG_RX_AWAKE(row)           IOCACHE_REG((row), IOCACHE_RX_AWAKE_OFF)
#define IOCACHE_REG_TXCOMP_AVAILABLE(row)   IOCACHE_REG((row), IOCACHE_TXCOMP_AVAILABLE_OFF)
#define IOCACHE_REG_TXCOMP_AWAKE(row)       IOCACHE_REG((row), IOCACHE_TXCOMP_AWAKE_OFF)
#define IOCACHE_REG_FLAGS(row)         		IOCACHE_REG((row), IOCACHE_FLAGS_OFF)

#define MAGIC_CHAR 0xCCCCCCCCUL

struct iocache_device {
	struct device *dev;
	
	int irq;

	resource_size_t hw_regs_control_size;
	phys_addr_t hw_regs_control_phys;
	void __iomem *iomem;

    struct miscdevice misc_dev; // Add the miscdevice member here	
	
	unsigned long magic;
};

static int iocache_misc_open(struct inode *inode, struct file *filp);
static int iocache_misc_mmap(struct file *filp, struct vm_area_struct *vma);
static int iocache_misc_release(struct inode *inode, struct file *filp);
static long iocache_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* IOCACHE_H */