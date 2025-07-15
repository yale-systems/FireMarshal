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


/* Can't add new CONFIG parameters in an external module, so define them here */
#define CONFIG_ACCNET_MTU 1500
#define CONFIG_ACCNET_RING_SIZE 64
#define CONFIG_ACCNET_TX_THRESHOLD 16

#define ACCNET_NAME "acc-nic"

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

#define ETH_HEADER_BYTES 14
#define ALIGN_BYTES 8
#define ALIGN_MASK 0x7
#define ALIGN_SHIFT 3
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

typedef enum {
	Control,
	RxEngine,
	TxEngine
} AccNICDevice;

struct accnet_device {
	struct device *dev;
	void __iomem *iomem;
	void __iomem *iomem_tx;
	void __iomem *iomem_rx;
	struct napi_struct napi;
	struct sk_buff_cq send_cq;
	struct sk_buff_cq recv_cq;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	int tx_irq;
	int rx_irq;

    // DMA buffer TX
	size_t dma_region_len_tx;
	void *dma_region_tx;
	dma_addr_t dma_region_addr_tx;

	// DMA buffer RX
	size_t dma_region_len_rx;
	void *dma_region_rx;
	dma_addr_t dma_region_addr_rx;

	// DMA buffer UDP TX
	size_t dma_region_len_udp_tx;
	void *dma_region_udp_tx;
	dma_addr_t dma_region_addr_udp_tx;

	// DMA buffer UDP RX
	size_t dma_region_len_udp_rx;
	void *dma_region_udp_rx;
	dma_addr_t dma_region_addr_udp_rx;

    struct miscdevice misc_dev; // Add the miscdevice member here
};

#endif /* ACCNET_DRIVER_H */