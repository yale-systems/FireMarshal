// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (C) 2017-2019 Regents of the University of California
 */

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

#include "accnet.h"

MODULE_DESCRIPTION("accnet driver");
MODULE_AUTHOR("Amirmohammad Nazari");
MODULE_LICENSE("Dual MIT/GPL");


void print_skb_data(struct sk_buff *skb) {
    unsigned int len = skb_headlen(skb); // Only prints linear part, not fragments

    printk(KERN_DEBUG "skb->len = %u\n", skb->len);
    printk(KERN_DEBUG "skb->headlen = %u\n", len);
    printk(KERN_DEBUG "skb->data:\n");

    // Print linear data
#ifdef DEBUG
    int i;
    unsigned int total = 0;
	unsigned char *data;
    data = skb->data;
    for (i = 0; i < len && total < 128; i++, total++)
        printk(KERN_CONT "%02x ", data[i]);
#endif
}



static inline void sk_buff_cq_init(struct sk_buff_cq *cq)
{
	cq->head = 0;
	cq->tail = 0;
}

static inline void sk_buff_cq_push(
		struct sk_buff_cq *cq, struct sk_buff *skb)
{
	if ( ((cq->head + 1) & (CONFIG_ACCNET_RING_SIZE - 1)) == cq->tail ) {
		printk(KERN_ERR "AccNet: sk_buff_cq_push overflow\n");
		return;
	}
	cq->entries[cq->head].skb = skb;
	cq->head = (cq->head + 1) & (CONFIG_ACCNET_RING_SIZE - 1);
}

static inline struct sk_buff *sk_buff_cq_pop(struct sk_buff_cq *cq)
{
	if (cq->head == cq->tail) {
		printk(KERN_ERR "AccNet: sk_buff_cq_pop underflow\n");
		return NULL;
	}

	struct sk_buff *skb;

	skb = cq->entries[cq->tail].skb;
	cq->tail = (cq->tail + 1) & (CONFIG_ACCNET_RING_SIZE - 1);

	return skb;
}

static inline int sk_buff_cq_tail_nsegments(struct sk_buff_cq *cq)
{
	struct sk_buff *skb;

	skb = cq->entries[cq->tail].skb;

	return skb_shinfo(skb)->nr_frags + 1;
}

static inline int send_req_avail(struct accnet_device *nic)
{
	return (64 - (ioread16(nic->iomem_tx + ACCNET_TX_REQ_COUNT) & 0xffff));
}

static inline int recv_req_avail(struct accnet_device *nic)
{
	return (64 - (ioread16(nic->iomem_rx + ACCNET_RX_DMA_ADDR_COUNT) & 0xffff));
}

static inline int send_comp_avail(struct accnet_device *nic)
{
	return (ioread16(nic->iomem_tx + ACCNET_TX_COUNT) & 0xffff);
}

static inline int recv_comp_avail(struct accnet_device *nic)
{
	return (ioread16(nic->iomem_rx + ACCNET_RX_COMP_COUNT) & 0xffff);
}

static inline void set_intmask(struct accnet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ACCNET_INTR_MASK;
	atomic_fetch_or(mask, mem);
}

static inline void clear_intmask(struct accnet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ACCNET_INTR_MASK;
	atomic_fetch_and(~mask, mem);
}

static inline void post_send_frag(
		struct accnet_device *nic, skb_frag_t *frag, int last)
{
	uintptr_t addr = page_to_phys(frag->bv_page) + frag->bv_offset;
	uint64_t len = frag->bv_len, partial = !last, packet;

	packet = (partial << 63) | (len << 48) | (addr & 0xffffffffffffL);
	iowrite64(packet, nic->iomem_tx + ACCNET_TX_REQ);
}

static inline void post_send(
		struct accnet_device *nic, struct sk_buff *skb)
{
	uintptr_t addr = virt_to_phys(skb->data);
	uint64_t len, partial, packet;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	if (shinfo->nr_frags > 0) {
		len = skb_headlen(skb);
		partial = 1;
	} else {
		len = skb->len;
		partial = 0;
	}

	packet = (partial << 63) | (len << 48) | (addr & 0xffffffffffffL);
	iowrite64(packet, nic->iomem_tx + ACCNET_TX_REQ);

	for (i = 0; i < shinfo->nr_frags; i++) {
		skb_frag_t *frag = &shinfo->frags[i];
		int last = i == (shinfo->nr_frags-1);
		post_send_frag(nic, frag, last);
	}

	sk_buff_cq_push(&nic->send_cq, skb);

	printk(KERN_DEBUG "AccNet: tx addr=%lx len=%llu\n", addr, len);
}

static inline void post_recv(
		struct accnet_device *nic, struct sk_buff *skb)
{
	int align = DMA_PTR_ALIGN(skb->data) - skb->data;
	uintptr_t addr;

	skb_reserve(skb, align);
	addr = virt_to_phys(skb->data);

	dev_dbg(nic->dev, "Posting receive buffer at phys_addr=%lx with alignment %d\n", addr, align);
	iowrite64(addr, nic->iomem_rx + ACCNET_RX_DMA_ADDR);
	sk_buff_cq_push(&nic->recv_cq, skb);
}

static inline int send_space(struct accnet_device *nic, int nfrags)
{
	return (send_req_avail(nic) >= nfrags) ? 1 : 0;
}

static void complete_send(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	int i, n, nsegs;

	dev_dbg(nic->dev, "Completing send requests\n");

	n = send_comp_avail(nic);
	dev_dbg(nic->dev, "Completing %d send requests\n", n);

	for (; n > 0; n -= nsegs) {
		nsegs = sk_buff_cq_tail_nsegments(&nic->send_cq);

		if (nsegs > n)
			break;

		dev_dbg(nic->dev, "Processing %d segments\n", nsegs);
		for (i = 0; i < nsegs; i++)
			ioread16(nic->iomem_tx + ACCNET_TX_COMP_READ);

		dev_dbg(nic->dev, "Popping %d segments from send_cq\n", nsegs);
		skb = sk_buff_cq_pop(&nic->send_cq);
		dev_consume_skb_irq(skb);
	}

	if (send_space(nic, MAX_SKB_FRAGS) && netif_queue_stopped(ndev)) {
		dev_info(nic->dev, "starting queue\n");
		netif_wake_queue(ndev);
	}
}

static int complete_recv(struct net_device *ndev, int budget)
{
	struct accnet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	int len, n, i;
	uint64_t res;
	uintptr_t addr;
	
	printk(KERN_DEBUG "AccNet: complete_recv called with budget %d\n", budget);
	n = recv_comp_avail(nic);
	if (budget < n) {
		n = budget;
	}
	printk(KERN_DEBUG "AccNet: Completing %d receive requests\n", n);

	for (i = 0; i < n; i++) {
		res = ioread64(nic->iomem_rx + ACCNET_RX_COMP_LOG);
		len = res & 0xffff;
		addr = (res >> 16) & 0xffffffffffffL;
		dev_dbg(nic->dev, "Received packet at phys_addr=%lx, virt_addr=%p, len=%d\n", addr, phys_to_virt(addr), len);

		skb = sk_buff_cq_pop(&nic->recv_cq);
		dev_dbg(nic->dev, "skb from recv_cq virt_addr=%p, phys_addr=%lx\n", skb->data, virt_to_phys(skb->data));


		skb_put(skb, len);
		print_skb_data(skb);

#ifdef CONFIG_ACCNET_CHECKSUM
		csum_res = ioread8(nic->iomem + ACCNET_RXCSUM_RES);
		if (csum_res == 3)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else if (csum_res == 1)
			printk(KERN_ERR "AccNet: Checksum offload detected incorrect checksum\n");
#endif
		skb->dev = ndev;
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		netif_receive_skb(skb);

		printk(KERN_DEBUG "AccNet: rx addr=%p, len=%d\n",
				skb->data, len);
	}

	return n;
}

static void alloc_recv(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	
	dev_dbg(nic->dev, "Allocating receive buffers\n");
	int recv_cnt = recv_req_avail(nic);
	
	dev_dbg(nic->dev, "Allocating %d receive buffers\n", recv_cnt);
	for ( ; recv_cnt > 0; recv_cnt--) {
		struct sk_buff *skb;
		skb = netdev_alloc_skb(ndev, DMA_LEN_ALIGN(MAX_FRAME_SIZE));
		post_recv(nic, skb);
	}
}

static inline void accnet_schedule(struct accnet_device *nic)
{
	struct napi_struct *napi = &nic->napi;
	if (likely(napi_schedule_prep(napi))) {
		__napi_schedule(napi);
	}
}


static irqreturn_t accnet_tx_isr(int irq, void *data)
{
	printk(KERN_DEBUG "TX interrupt received\n");
	struct net_device *ndev = data;
	struct accnet_device *nic = netdev_priv(ndev);

	if (irq != nic->tx_irq)
		return IRQ_NONE;

	spin_lock(&nic->tx_lock);
	clear_intmask(nic, ACCNET_INTMASK_TX);
	iowrite8(0x1, nic->iomem_tx + ACCNET_TX_INTR_CLEAR);
	spin_unlock(&nic->tx_lock);

	accnet_schedule(nic);

	return IRQ_HANDLED;
}

static irqreturn_t accnet_rx_isr(int irq, void *data)
{
	printk(KERN_DEBUG "RX interrupt received\n");
	struct net_device *ndev = data;
	struct accnet_device *nic = netdev_priv(ndev);

	if (irq != nic->rx_irq)
		return IRQ_NONE;

	spin_lock(&nic->rx_lock);
	clear_intmask(nic, ACCNET_INTMASK_RX);
	iowrite8(0x1, nic->iomem_rx + ACCNET_RX_INTR_CLEAR);
	spin_unlock(&nic->rx_lock);

	accnet_schedule(nic);

	return IRQ_HANDLED;
}

static int accnet_poll(struct napi_struct *napi, int budget)
{
	printk(KERN_DEBUG "NAPI poll called with budget %d\n", budget);
	struct accnet_device *nic;
	struct net_device *ndev;
	int work_done;
	unsigned long flags;

	nic = container_of(napi, struct accnet_device, napi);
	ndev = dev_get_drvdata(nic->dev);

	dev_dbg(nic->dev, "Processing AccNet device %s\n", ndev->name);
	spin_lock_irqsave(&nic->tx_lock, flags);
	complete_send(ndev);
	spin_unlock_irqrestore(&nic->tx_lock, flags);
	
	dev_dbg(nic->dev, "Processing AccNet RX buffers\n");
	work_done = complete_recv(ndev, budget);
	alloc_recv(ndev);

	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		set_intmask(nic, ACCNET_INTMASK_BOTH);
	}

	return work_done;
}

static int accnet_parse_addr(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	struct resource regs;
	struct device_node *np_rx, *np_tx;
	struct resource regs_rx, regs_tx;
	int err;

	err = of_address_to_resource(node, 0, &regs);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	nic->iomem = devm_ioremap_resource(dev, &regs);
	if (IS_ERR(nic->iomem)) {
		dev_err(dev, "could not remap io address %llx", regs.start);
		return PTR_ERR(nic->iomem);
	}

	// Find rx node
    np_rx = of_find_compatible_node(NULL, NULL, "yale-systems,acc-nic-rx-engine");
	if (!np_rx) {
		dev_err(dev, "Cannot find RX engine node\n");
		return -ENODEV;
	}

	if (of_address_to_resource(np_rx, 0, &regs_rx)) {
		dev_err(dev, "Failed to get RX resource\n");
		return -EINVAL;
	}

	nic->iomem_rx = devm_ioremap_resource(dev, &regs_rx);
	if (IS_ERR(nic->iomem_rx)) {
		dev_err(dev, "could not remap RX io address %llx", regs_rx.start);
		return PTR_ERR(nic->iomem_rx);
	}

	// Find tx node
    np_tx = of_find_compatible_node(NULL, NULL, "yale-systems,acc-nic-tx-engine");
	if (!np_tx) {
		dev_err(dev, "Cannot find TX engine node\n");
		return -ENODEV;
	}

	if (of_address_to_resource(np_tx, 0, &regs_tx)) {
		dev_err(dev, "Failed to get TX resource\n");
		return -EINVAL;
	}

	nic->iomem_tx = devm_ioremap_resource(dev, &regs_tx);
	if (IS_ERR(nic->iomem_tx)) {
		dev_err(dev, "could not remap TX io address %llx", regs_tx.start);
		return PTR_ERR(nic->iomem_tx);
	}

	return 0;
}

static int accnet_parse_irq(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	int err;

	const char *name = of_node_full_name(node);
	dev_info(dev, "Device Tree node: %s\n", name);

	nic->tx_irq = irq_of_parse_and_map(node, 0);
	if (!nic->tx_irq) {
    	dev_err(dev, "Failed to parse TX IRQ from DT\n");
		return -EINVAL;
	}
	
	dev_info(dev, "Requesting TX IRQ %d\n", nic->tx_irq);
	err = devm_request_irq(dev, nic->tx_irq, accnet_tx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ACCNET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain TX irq %d\n", nic->tx_irq);
		return err;
	}

	nic->rx_irq = irq_of_parse_and_map(node, 1);
	if (!nic->rx_irq) {
    	dev_err(dev, "Failed to parse RX IRQ from DT\n");
		return -EINVAL;
	}
	
	dev_info(dev, "Requesting RX IRQ %d\n", nic->rx_irq);
	err = devm_request_irq(dev, nic->rx_irq, accnet_rx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ACCNET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain RX irq %d\n", nic->rx_irq);
		return err;
	}

	return 0;
}

static int accnet_open(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	unsigned long flags;

#ifdef CONFIG_ACCNET_CHECKSUM
	iowrite8(1, nic->iomem + ACCNET_CSUM_ENABLE);
	mb();
#endif

	dev_info(nic->dev, "Opening AccNet device %s\n", ndev->name);

	napi_enable(&nic->napi);

	alloc_recv(ndev);

	dev_info(nic->dev, "Starting Tx queue\n");
	netif_start_queue(ndev);

	
	dev_info(nic->dev, "Enabling RX interrupts\n");
	spin_lock_irqsave(&nic->rx_lock, flags);
	set_intmask(nic, ACCNET_INTMASK_RX);
	spin_unlock_irqrestore(&nic->rx_lock, flags);

	dev_info(nic->dev, "Enabling TX interrupts\n");
	spin_lock_irqsave(&nic->tx_lock, flags);
	set_intmask(nic, ACCNET_INTMASK_TX);
	spin_unlock_irqrestore(&nic->tx_lock, flags);

	dev_info(nic->dev, "AccNet device %s opened successfully\n", ndev->name);
	return 0;
}

static int accnet_stop(struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);

	napi_disable(&nic->napi);

	clear_intmask(nic, ACCNET_INTMASK_BOTH);
	netif_stop_queue(ndev);

	dev_info(nic->dev, "AccNet device %s closed successfully\n", ndev->name);
	return 0;
}

static int accnet_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct accnet_device *nic = netdev_priv(ndev);
	unsigned long flags;

	dev_dbg(nic->dev, "Transmitting packet of length %d\n", skb->len);
	print_skb_data(skb);

	spin_lock_irqsave(&nic->tx_lock, flags);

	if (unlikely(!send_space(nic, skb_shinfo(skb)->nr_frags + 1))) {
		netif_stop_queue(ndev);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		spin_unlock_irqrestore(&nic->tx_lock, flags);
		netdev_err(ndev, "insufficient space in Tx ring\n");
		return NETDEV_TX_BUSY;
	}

	skb_tx_timestamp(skb);
	post_send(nic, skb);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	if (send_comp_avail(nic) > CONFIG_ACCNET_TX_THRESHOLD) {
		accnet_schedule(nic);
	}

	if (unlikely(!send_space(nic, MAX_SKB_FRAGS))) {
		netif_stop_queue(ndev);
		accnet_schedule(nic);
	}

	spin_unlock_irqrestore(&nic->tx_lock, flags);

	return NETDEV_TX_OK;
}

static void accnet_init_mac_address(struct net_device *ndev)
{
	// struct accnet_device *nic = netdev_priv(ndev);
	
	// Hard-Coding MacAddress for now
	// uint64_t macaddr = ioread64(nic->iomem + ACCNET_MACADDR);
	uint64_t macaddr = 0x112233445566;

	ndev->addr_assign_type = NET_ADDR_PERM;
	ndev->addr_len = MACADDR_BYTES;
	dev_addr_set(ndev, (void*)&macaddr);

	if (!is_valid_ether_addr(ndev->dev_addr)) {
		// printk(KERN_WARNING "Invalid MAC address\n");
		printk(KERN_WARNING "Invalid MAC address: 0x%llx\n", macaddr);
	}
}

static const struct net_device_ops accnet_ops = {
	.ndo_open = accnet_open,
	.ndo_stop = accnet_stop,
	.ndo_start_xmit = accnet_start_xmit
};

static int accnet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct accnet_device *nic;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	ndev = devm_alloc_etherdev(dev, sizeof(struct accnet_device));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	nic = netdev_priv(ndev);
	nic->dev = dev;

	netif_napi_add(ndev, &nic->napi, accnet_poll);

	ether_setup(ndev);
	ndev->flags &= ~IFF_MULTICAST;
	ndev->netdev_ops = &accnet_ops;
	ndev->hw_features = NETIF_F_SG;

	ndev->features = ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	ndev->max_mtu = CONFIG_ACCNET_MTU;

	spin_lock_init(&nic->tx_lock);
	spin_lock_init(&nic->rx_lock);

	sk_buff_cq_init(&nic->send_cq);
	sk_buff_cq_init(&nic->recv_cq);

	if ((ret = accnet_parse_addr(ndev)) < 0)
		return ret;

	accnet_init_mac_address(ndev);

	if ((ret = register_netdev(ndev)) < 0) {
		dev_err(dev, "Failed to register netdev\n");
		return ret;
	}

	if ((ret = accnet_parse_irq(ndev)) < 0)
		return ret;

	printk(KERN_INFO "Registered AccNet NIC %02x:%02x:%02x:%02x:%02x:%02x\n",
			ndev->dev_addr[0],
			ndev->dev_addr[1],
			ndev->dev_addr[2],
			ndev->dev_addr[3],
			ndev->dev_addr[4],
			ndev->dev_addr[5]);

	return 0;
}

static int accnet_remove(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct accnet_device *nic;

	ndev = platform_get_drvdata(pdev);
	nic = netdev_priv(ndev);
	netif_napi_del(&nic->napi);
	unregister_netdev(ndev);

	return 0;
}

static struct of_device_id accnet_of_match[] = {
	{ .compatible = "yale-systems,acc-nic" },
	{ .compatible = "yale-systems,accnic" },
	{}
};

static struct platform_driver accnet_driver = {
	.driver = {
		.name = ACCNET_NAME,
		.of_match_table = accnet_of_match,
		.suppress_bind_attrs = true
	},
	.probe = accnet_probe,
	.remove = accnet_remove
};

module_platform_driver(accnet_driver);
MODULE_DESCRIPTION("Drives the FireChip AccNIC ethernet device (used in firesim)");
MODULE_LICENSE("GPL");
// MODULE_LACCNSE("GPL");