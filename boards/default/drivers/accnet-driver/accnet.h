#ifndef __ACCNET_DRIVER_H
#define __ACCNET_DRIVER_H

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
#define REG(base, off) ((void __iomem *)((u8 __iomem *)(base) + (off)))

/* Can't add new CONFIG parameters in an external module, so define them here */
#define CONFIG_ACCNET_MTU 1500
#define CONFIG_ACCNET_RING_SIZE 1280
#define CONFIG_ACCNET_TX_THRESHOLD 16

#define ACCNET_NAME "accnet"

#define ACCNET_INTMASK_TX 1
#define ACCNET_INTMASK_RX 2
#define ACCNET_INTMASK_BOTH 3

#define ACCNET_BASE 0x0
#define ACCNET_CTRL_OFFSET 0x0000
#define ACCNET_RX_OFFSET 0x0000
#define ACCNET_TX_OFFSET 0x0000

#define ACCNET_RX_BASE (ACCNET_BASE + ACCNET_RX_OFFSET)
#define ACCNET_TX_BASE (ACCNET_BASE + ACCNET_TX_OFFSET)
#define ACCNET_CTRL_BASE (ACCNET_BASE + ACCNET_CTRL_OFFSET)

// Control registers
#define ACCNET_INTR_MASK 		(ACCNET_CTRL_BASE + 0x00)
#define ACCNET_CTRL_TIMESTAMP   (ACCNET_CTRL_BASE + 0x10)

// RX Engine registers
#define ACCNET_RX_DMA_ADDR_COUNT 		0x04
#define ACCNET_RX_DMA_ADDR 				0x08
#define ACCNET_RX_COMP_LOG 				0x10
#define ACCNET_RX_COMP_COUNT 			0x18
#define ACCNET_RX_INTR_PEND 			0x20
#define ACCNET_RX_INTR_CLEAR 			0x24

// TX Engine registers
#define ACCNET_TX_REQ_COUNT 			0x00
#define ACCNET_TX_REQ 					0x08
#define ACCNET_TX_COUNT 				0x10
#define ACCNET_TX_COMP_READ 			0x12
#define ACCNET_TX_INTR_PEND 			0x18
#define ACCNET_TX_INTR_CLEAR 			0x1C

/* =========================  UDP  =========================== */
#define ACCNET_UDP_RING_SIZE 	32 * 1024 		// 64KB
#define ACCNET_UDP_RING_COUNT	64

/* ===================================================================== */
/* =========================  UDP RX ENGINE  =========================== */
/* ===================================================================== */
#define ACCNET_UDP_RX_RING_STRIDE          0x20UL

#define ACCNET_UDP_RX_RING_HEAD_OFF        		0x00UL
#define ACCNET_UDP_RX_RING_TAIL_OFF        		0x04UL
#define ACCNET_UDP_RX_RING_DROP_OFF        		0x08UL
#define ACCNET_UDP_RX_RING_LAST_TIMESTAMP_OFF  	0x10UL


#define ACCNET_UDP_RX_RING_REG(r, off)    ((uint64_t)(r) * ACCNET_UDP_RX_RING_STRIDE + (off))

/* Per-ring convenience */
#define ACCNET_UDP_RX_RING_HEAD(r)        		ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_HEAD_OFF)   /* 32-bit */
#define ACCNET_UDP_RX_RING_TAIL(r)        		ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_TAIL_OFF)   /* 32-bit */
#define ACCNET_UDP_RX_RING_DROP(r)        		ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_DROP_OFF)   /* 32-bit, RO */
#define ACCNET_UDP_RX_RING_LAST_TIMESTAMP(r)    ACCNET_UDP_RX_RING_REG((r), ACCNET_UDP_RX_RING_LAST_TIMESTAMP_OFF)   /* 64-bit, RO */

/* Engine-level IRQ */
#define ACCNET_UDP_RX_IRQ_PENDING         0x400UL
#define ACCNET_UDP_RX_IRQ_CLEAR           0x404UL

/* ===================================================================== */
/* =========================  UDP TX ENGINE  =========================== */
/* ===================================================================== */
#define ACCNET_UDP_TX_RING_STRIDE          0x10UL

#define ACCNET_UDP_TX_RING_HEAD_OFF        		0x00UL
#define ACCNET_UDP_TX_RING_TAIL_OFF        		0x04UL
#define ACCNET_UDP_TX_RING_LAST_TIMESTAMP_OFF   0x08UL

#define ACCNET_UDP_TX_RING_REG(r, off)    ((uint64_t)(r) * ACCNET_UDP_TX_RING_STRIDE + (off))

/* Per-ring convenience */
#define ACCNET_UDP_TX_RING_HEAD(r)        		ACCNET_UDP_TX_RING_REG((r), ACCNET_UDP_TX_RING_HEAD_OFF)   /* 32-bit */
#define ACCNET_UDP_TX_RING_TAIL(r)        		ACCNET_UDP_TX_RING_REG((r), ACCNET_UDP_TX_RING_TAIL_OFF)   /* 32-bit */
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

// Others
#define ETH_HEADER_BYTES 14
#define ALIGN_BYTES 64
#define ALIGN_MASK 0x3f
#define ALIGN_SHIFT 6
#define MAX_FRAME_SIZE (CONFIG_ACCNET_MTU + ETH_HEADER_BYTES + NET_IP_ALIGN)
#define DMA_PTR_ALIGN(p) ((typeof(p)) (__ALIGN_KERNEL((uintptr_t) (p), ALIGN_BYTES)))
#define DMA_LEN_ALIGN(n) (((((n) - 1) >> ALIGN_SHIFT) + 1) << ALIGN_SHIFT)
#define MACADDR_BYTES 6

struct sk_buff_cq_entry {
	struct sk_buff *skb;
};

struct sk_buff_cq {
	struct sk_buff_cq_entry entries[CONFIG_ACCNET_RING_SIZE];
	int head;
	int tail;
};

#define SK_BUFF_CQ_COUNT(cq) CIRC_CNT(cq.head, cq.tail, CONFIG_ACCNET_RING_SIZE)
#define SK_BUFF_CQ_SPACE(cq) CIRC_SPACE(cq.head, cq.tail, CONFIG_ACCNET_RING_SIZE)

#define MAGIC_CHAR 0xCCCCCCCCUL

typedef enum {
	Control,
	RxEngine,
	TxEngine
} AccNICDevice;

struct accnet_device {
	struct device *dev;
	struct napi_struct napi;
	struct sk_buff_cq send_cq;
	struct sk_buff_cq recv_cq;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	int tx_irq;
	int rx_irq;

	resource_size_t hw_regs_control_size;
	phys_addr_t hw_regs_control_phys;
	void __iomem *iomem;
	void __iomem *iomem_tx;
	void __iomem *iomem_rx;

	resource_size_t hw_regs_udp_tx_size;
	phys_addr_t hw_regs_udp_tx_phys;
	void __iomem *iomem_udp_tx;

	resource_size_t hw_regs_udp_rx_size;
	phys_addr_t hw_regs_udp_rx_phys;
	void __iomem *iomem_udp_rx;

    struct miscdevice misc_dev; // Add the miscdevice member here

	unsigned long magic;
};

static int accnet_misc_mmap(struct file *filp, struct vm_area_struct *vma);
static int accnet_misc_open(struct inode *inode, struct file *filp);
static int accnet_misc_release(struct inode *inode, struct file *filp);
static long accnet_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif /* __ACCNET_DRIVER_H */