#ifndef ACCNET_DRIVER_H
#define ACCNET_DRIVER_H

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

// #define ACCNIC_BASE 0x10018000L
#define ACCNET_BASE 0x0
#define ACCNET_CTRL_OFFSET 0x0000
#define ACCNET_RX_OFFSET 0x0000
#define ACCNET_TX_OFFSET 0x0000

#define ACCNET_RX_BASE (ACCNET_BASE + ACCNET_RX_OFFSET)
#define ACCNET_TX_BASE (ACCNET_BASE + ACCNET_TX_OFFSET)
#define ACCNET_CTRL_BASE (ACCNET_BASE + ACCNET_CTRL_OFFSET)

// Control registers
#define ACCNET_INTR_MASK (ACCNET_CTRL_BASE + 0x00)

// RX Engine registers
#define ACCNET_RX_DMA_ADDR_COUNT (ACCNET_RX_BASE + 0x04)
#define ACCNET_RX_DMA_ADDR (ACCNET_RX_BASE + 0x08)
#define ACCNET_RX_COMP_LOG (ACCNET_RX_BASE + 0x10)
#define ACCNET_RX_COMP_COUNT (ACCNET_RX_BASE + 0x18)
#define ACCNET_RX_INTR_PEND (ACCNET_RX_BASE + 0x20)
#define ACCNET_RX_INTR_CLEAR (ACCNET_RX_BASE + 0x24)

// TX Engine registers
#define ACCNET_TX_REQ_COUNT (ACCNET_TX_BASE + 0x00)
#define ACCNET_TX_REQ (ACCNET_TX_BASE + 0x08)
#define ACCNET_TX_COUNT (ACCNET_TX_BASE + 0x10)
#define ACCNET_TX_COMP_READ (ACCNET_TX_BASE + 0x12)
#define ACCNET_TX_INTR_PEND (ACCNET_TX_BASE + 0x18)
#define ACCNET_TX_INTR_CLEAR (ACCNET_TX_BASE + 0x1C)

/* UDP */
#define ACCNET_UDP_RING_SIZE 16 * 1024

// UDP RX Engine registers
#define ACCNET_UDP_RX_RING_BASE 0x00
#define ACCNET_UDP_RX_RING_SIZE 0x08
#define ACCNET_UDP_RX_RING_HEAD 0x0C
#define ACCNET_UDP_RX_RING_TAIL 0x10

// UDP TX Engine registers
#define ACCNET_UDP_TX_RING_BASE        0x00
#define ACCNET_UDP_TX_RING_SIZE        0x08
#define ACCNET_UDP_TX_RING_HEAD        0x0C
#define ACCNET_UDP_TX_RING_TAIL        0x10
#define ACCNET_UDP_TX_MTU              0x14
#define ACCNET_UDP_TX_HDR_MAC_SRC      0x20
#define ACCNET_UDP_TX_HDR_MAC_DST      0x28
#define ACCNET_UDP_TX_HDR_IP_SRC       0x30
#define ACCNET_UDP_TX_HDR_IP_DST       0x34
#define ACCNET_UDP_TX_HDR_IP_TOS       0x38
#define ACCNET_UDP_TX_HDR_IP_TTL       0x39
#define ACCNET_UDP_TX_HDR_IP_ID        0x3A
#define ACCNET_UDP_TX_HDR_UDP_SRC_PORT 0x40
#define ACCNET_UDP_TX_HDR_UDP_DST_PORT 0x42
#define ACCNET_UDP_TX_HDR_UDP_CSUM     0x44

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
	void __iomem *iomem_tx;
	void __iomem *iomem_rx;
	struct napi_struct napi;
	struct sk_buff_cq send_cq;
	struct sk_buff_cq recv_cq;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	int tx_irq;
	int rx_irq;

	// DMA buffer UDP TX
	size_t dma_region_len_udp_tx;
	void *dma_region_udp_tx, *dma_region_udp_tx_aligned;
	dma_addr_t dma_region_addr_udp_tx, dma_region_addr_udp_tx_aligned;

	// DMA buffer UDP RX
	size_t dma_region_len_udp_rx;
	void *dma_region_udp_rx, *dma_region_udp_rx_aligned;
	dma_addr_t dma_region_addr_udp_rx, dma_region_addr_udp_rx_aligned;

	resource_size_t hw_regs_control_size;
	phys_addr_t hw_regs_control_phys;
	void __iomem *iomem;

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

#endif /* ACCNET_DRIVER_H */