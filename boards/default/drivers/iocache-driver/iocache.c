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
#include <linux/of.h>
#include <linux/io.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>

#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/task.h>   // current
#include <linux/smp.h>          // smp_processor_id(), smp_send_reschedule()

#include "iocache.h"
#include "iocache_ioctl.h"
#include "iocache_misc.c"
#include "iocache_plic.c"


MODULE_DESCRIPTION("iocache driver");
MODULE_AUTHOR("Amirmohammad Nazari");
MODULE_LICENSE("Dual MIT/GPL");


static inline void set_intmask_rx(struct iocache_device *iocache, int cpu)
{
	iowrite8(0x1, REG(iocache->iomem, IOCACHE_REG_INTMASK_RX(cpu)));
}

static inline void clear_intmask_rx(struct iocache_device *iocache, int cpu)
{
	iowrite8(0x0, REG(iocache->iomem, IOCACHE_REG_INTMASK_RX(cpu)));
}

static inline void set_intmask_txcomp(struct iocache_device *iocache, int cpu)
{
	iowrite8(0x1, REG(iocache->iomem, IOCACHE_REG_INTMASK_TXCOMP(cpu)));
}

static inline void clear_intmask_txcomp(struct iocache_device *iocache, int cpu)
{
	iowrite8(0x0, REG(iocache->iomem, IOCACHE_REG_INTMASK_TXCOMP(cpu)));
}

static const struct file_operations iocache_fops = {
    .owner = THIS_MODULE,
    .mmap = iocache_misc_mmap,
    .open = iocache_misc_open,
    .release = iocache_misc_release,
	.unlocked_ioctl = iocache_misc_ioctl,
};

extern u64 riscv_get_irq_entry_ktime(void);   // from do_irq patch
extern u64 riscv_get_plic_claim_ktime(void);  // new, from plic_handle_irq

static irqreturn_t iocache_isr_rx(int irq, void *data) {
	
	struct iocache_device *iocache = data;
	int cpu = smp_processor_id();
	
	// printk(KERN_INFO "RX interrupt received at cpu %d\n", cpu);

	iowrite32(cpu, REG(iocache->iomem, IOCACHE_REG_RX_KICK_ALL_CPU));
	mmiowb();

	// printk(KERN_INFO "Kick Count: %d\n", ioread8(REG(iocache->iomem, IOCACHE_REG_RX_KICK_ALL_COUNT)));
	// printk(KERN_INFO "Kick Mask : 0x%llX\n", ioread64(REG(iocache->iomem, IOCACHE_REG_RX_KICK_ALL_MASK)));
	// clear_intmask_rx(iocache, cpu);

	set_tsk_need_resched(current);

    return IRQ_HANDLED;
}

static irqreturn_t iocache_isr_txcomp(int irq, void *data) {
	// printk(KERN_DEBUG "TX interrupt received\n");
	// Nothing for now
	return IRQ_HANDLED;
}

static int iocache_parse_irq(struct iocache_device *iocache) {
	struct device *dev = iocache->dev;
	struct device_node *node = dev->of_node;
	cpumask_t mask;
	int err;

	const char *name = of_node_full_name(node);
	dev_info(dev, "Device Tree node: %s\n", name);

	for (uint32_t i = 0; i < NUM_CPUS; i++) {
		int rx_index = 2*i;
		int tx_index = 2*i + 1;

		iocache->rx_irq[i] = irq_of_parse_and_map(node, rx_index);
		if (!iocache->rx_irq[i]) {
			dev_err(dev, "Failed to parse RX IRQ from DT\n");
			return -EINVAL;
		}
		
		dev_info(dev, "Requesting RX IRQ %d\n", iocache->rx_irq[i]);
		err = devm_request_irq(dev, iocache->rx_irq[i], iocache_isr_rx,
				IRQF_SHARED | IRQF_NO_THREAD,
				IOCACHE_NAME, iocache);
		if (err) {
			dev_err(dev, "could not obtain rx irq %d\n", iocache->rx_irq[i]);
			return err;
		}

		/* Force the core to steer RX IRQ to CPU i */
		cpumask_clear(&mask);
		cpumask_set_cpu(i, &mask);
		irq_set_affinity(iocache->rx_irq[i], &mask);

		iocache->txcomp_irq[i] = irq_of_parse_and_map(node, tx_index);
		if (!iocache->txcomp_irq[i]) {
			dev_err(dev, "Failed to parse TX_COMP IRQ from DT\n");
			return -EINVAL;
		}
		
		dev_info(dev, "Requesting TX_COMP IRQ %d\n", iocache->txcomp_irq[i]);
		err = devm_request_irq(dev, iocache->txcomp_irq[i], iocache_isr_txcomp,
				IRQF_SHARED | IRQF_NO_THREAD,
				IOCACHE_NAME, iocache);
		if (err) {
			dev_err(dev, "could not obtain tx_comp irq %d\n", iocache->txcomp_irq[i]);
			return err;
		}
		/* Force the core to steer TX IRQ to CPU i */
		cpumask_clear(&mask);
		cpumask_set_cpu(i, &mask);
		irq_set_affinity(iocache->txcomp_irq[i], &mask);
	}

	return 0;
}

static int iocache_parse_addr(struct iocache_device *iocache) {
	struct device *dev = iocache->dev;
	struct device_node *node = dev->of_node;
	struct resource regs;
	int err;

	err = of_address_to_resource(node, 0, &regs);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	iocache->iomem = devm_ioremap_resource(dev, &regs);
	if (IS_ERR(iocache->iomem)) {
		dev_err(dev, "could not remap io address %llx", regs.start);
		return PTR_ERR(iocache->iomem);
	}

	iocache->hw_regs_control_phys = regs.start;                 /* <-- save phys */
	iocache->hw_regs_control_size = resource_size(&regs);       /* <-- save size  */

	return 0;
}

static int init_udp_rings(struct iocache_device *iocache) {
	struct device *dev = iocache->dev;
	int ret = 0;
	size_t delta;

	for (uint32_t i = 0; i < IOCACHE_CACHE_ENTRY_COUNT; i++) {
		// Allocate DMA buffer UDP TX
		iocache->dma_region_len_udp_tx[i] = IOCACHE_UDP_RING_SIZE;
		iocache->dma_region_udp_tx[i] = dma_alloc_coherent(dev, iocache->dma_region_len_udp_tx[i] + (ALIGN_BYTES - 1), 
													&iocache->dma_region_addr_udp_tx[i], GFP_KERNEL | __GFP_ZERO);
		if (!iocache->dma_region_udp_tx[i]) {
			dev_err(dev, "Failed to allocate DMA buffer UDP TX (%u)", i);
			ret = -ENOMEM;
			break;
		}
		iocache->dma_region_addr_udp_tx_aligned[i] = ALIGN(iocache->dma_region_addr_udp_tx[i], ALIGN_BYTES);
		delta       	= iocache->dma_region_addr_udp_tx_aligned[i] - iocache->dma_region_addr_udp_tx[i];
		iocache->dma_region_udp_tx_aligned[i] 		= (void *)((uintptr_t)iocache->dma_region_udp_tx[i] + delta);

		dev_info(dev, "Allocated DMA UDP_TX region (%u) virt %p, phys %p", i, 
				iocache->dma_region_udp_tx_aligned[i], (void *)iocache->dma_region_addr_udp_tx_aligned[i]);
		
		// Allocate DMA buffer UDP RX
		iocache->dma_region_len_udp_rx[i] = IOCACHE_UDP_RING_SIZE;
		iocache->dma_region_udp_rx[i] = dma_alloc_coherent(dev, iocache->dma_region_len_udp_rx[i] + (ALIGN_BYTES - 1), 
													&iocache->dma_region_addr_udp_rx[i], GFP_KERNEL | __GFP_ZERO);
		if (!iocache->dma_region_udp_rx[i]) {
			dev_err(dev, "Failed to allocate DMA buffer UDP RX (%u)", i);
			ret = -ENOMEM;
			break;
		}
		iocache->dma_region_addr_udp_rx_aligned[i] = ALIGN(iocache->dma_region_addr_udp_rx[i], ALIGN_BYTES);
		delta       	= iocache->dma_region_addr_udp_rx_aligned[i] - iocache->dma_region_addr_udp_rx[i];
		iocache->dma_region_udp_rx_aligned[i] 		= (void *)((uintptr_t)iocache->dma_region_udp_rx[i] + delta);

		dev_info(dev, "Allocated DMA UDP_RX region (%u) virt %p, phys %p", i,
				iocache->dma_region_udp_rx_aligned[i], (void *)iocache->dma_region_addr_udp_rx_aligned[i]);
	}

	return ret;
}

static void populate_ring_info(struct iocache_device *iocache) {
	for (uint32_t i = 0; i < IOCACHE_CACHE_ENTRY_COUNT; i++) {
		iowrite64(0, 		  		 	 REG(iocache->iomem, IOCACHE_REG_ENABLED(i)));
		/* RX */
		u64 rx_base = (u64)iocache->dma_region_addr_udp_rx_aligned[i];
		iowrite64(rx_base, 		  		 REG(iocache->iomem, IOCACHE_REG_RX_RING_ADDR(i)));
		iowrite32(IOCACHE_UDP_RING_SIZE, REG(iocache->iomem, IOCACHE_REG_RX_RING_SIZE(i)));

		/* TX ring */
		u64 tx_base = (u64)iocache->dma_region_addr_udp_tx_aligned[i];
		iowrite64(tx_base, 		  		 REG(iocache->iomem, IOCACHE_REG_TX_RING_ADDR(i)));
		iowrite32(IOCACHE_UDP_RING_SIZE, REG(iocache->iomem, IOCACHE_REG_TX_RING_SIZE(i)));
	}
}

static int iocache_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct iocache_device *iocache;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	iocache = devm_kzalloc(dev, sizeof(*iocache), GFP_KERNEL);
    if (!iocache) {
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, iocache);
	dev_set_drvdata(dev, iocache);
	iocache->dev = dev;
	iocache->magic = MAGIC_CHAR;

	if ((ret = iocache_parse_addr(iocache)) < 0)
		return ret;

	if ((ret = iocache_parse_irq(iocache)) < 0)
		return ret;
	
	if ((ret = plic_set_prio(iocache)) != 0)
		return ret;

	spin_lock_init(&iocache->ev_lock);
	u64_stats_init(&iocache->syncp);

	register_iocache_forall(iocache->iomem);

	ret = init_udp_rings(iocache);
	if (ret != 0) {
		return ret;
	}
	populate_ring_info(iocache);

	// plic_unregister_fast_path(iocache);
	if ((ret = plic_register_fast_path(iocache, iocache_isr_rx, iocache_isr_txcomp)) != 0)
		return ret;

	/* Register the misc device */
    iocache->misc_dev.minor = MISC_DYNAMIC_MINOR;
    iocache->misc_dev.name = "iocache-misc";
    iocache->misc_dev.fops = &iocache_fops;
	iocache->misc_dev.parent = dev;

	ret = misc_register(&iocache->misc_dev);
	dev_info(dev, "Misc registered :)");

	return 0;
}

static int iocache_remove(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct iocache_device *iocache = platform_get_drvdata(pdev);

    if (!iocache) {
		dev_warn(dev, "Could not find misc device to deregister");
        return 0;
	}
	
	plic_unregister_fast_path(iocache);

    misc_deregister(&iocache->misc_dev);

	for (uint32_t i = 0; i < NUM_CPUS; i++) {
		clear_intmask_rx(iocache, i);
		clear_intmask_txcomp(iocache, i);
	}

	
	// TODO: unregister iocache for rq

	for (uint32_t i = 0; i < IOCACHE_CACHE_ENTRY_COUNT; i++) {
		if (iocache->dma_region_udp_rx[i])
			dma_free_coherent(dev, 
				iocache->dma_region_len_udp_rx[i] + (ALIGN_BYTES - 1), 
				iocache->dma_region_udp_rx[i], 
				iocache->dma_region_addr_udp_rx[i]);
		if (iocache->dma_region_udp_tx[i])
			dma_free_coherent(dev, 
				iocache->dma_region_len_udp_tx[i] + (ALIGN_BYTES - 1), 
				iocache->dma_region_udp_tx[i], 
				iocache->dma_region_addr_udp_tx[i]);
	}

	return 0;
}

static struct of_device_id iocache_of_match[] = {
	{ .compatible = "yale-systems,iocache" },
	{}
};

static struct platform_driver iocache_driver = {
	.driver = {
		.name = IOCACHE_NAME,
		.of_match_table = iocache_of_match,
		.suppress_bind_attrs = true
	},
	.probe = iocache_probe,
	.remove = iocache_remove
};

module_platform_driver(iocache_driver);
MODULE_DESCRIPTION("Drives the FireChip AccNIC ethernet device (used in firesim)");
MODULE_LICENSE("GPL");