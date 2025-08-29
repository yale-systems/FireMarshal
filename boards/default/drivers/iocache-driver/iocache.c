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

#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>

#include "iocache_misc.c"
#include "iocache_ioctl.h"
#include "iocache.h"

MODULE_DESCRIPTION("iocache driver");
MODULE_AUTHOR("Amirmohammad Nazari");
MODULE_LICENSE("Dual MIT/GPL");

static inline void set_intmask_rx(struct iocache_device *iocache, uint32_t mask)
{
	iowrite8(0x1, REG(iocache->iomem, IOCACHE_REG_INTMASK_RX));
}

static inline void clear_intmask_rx(struct iocache_device *iocache, uint32_t mask)
{
	iowrite8(0x0, REG(iocache->iomem, IOCACHE_REG_INTMASK_RX));
}

static inline void set_intmask_txcomp(struct iocache_device *iocache, uint32_t mask)
{
	iowrite8(0x1, REG(iocache->iomem, IOCACHE_REG_INTMASK_TXCOMP));
}

static inline void clear_intmask_txcomp(struct iocache_device *iocache, uint32_t mask)
{
	iowrite8(0x0, REG(iocache->iomem, IOCACHE_REG_INTMASK_TXCOMP));
}

static const struct file_operations iocache_fops = {
    .owner = THIS_MODULE,
    .mmap = iocache_misc_mmap,
    .open = iocache_misc_open,
    .release = iocache_misc_release,
	.unlocked_ioctl = iocache_misc_ioctl,
};

static irqreturn_t iocache_isr_rx(int irq, void *data) {
	printk(KERN_DEBUG "RX interrupt received\n");

	struct device *dev = data;
	struct iocache_device *iocache = dev_get_drvdata(dev);
    struct eventfd_ctx *ctx = NULL;

	// save timestamp
	u64 now = ktime_get_mono_fast_ns();   // OK in hard IRQ
    iocache->last_irq_ns = now;

	clear_intmask_rx(iocache, IOCACHE_INTMASK_RX);

	/* Update shared state first, then wake */
    atomic_set(&iocache->ready, 1);
    /* Safe from hard IRQ context */
    wake_up_interruptible(&iocache->wq);	

	// spin_lock(&iocache->ev_lock);
    // ctx = iocache->ev_ctx;
    // if (ctx)
    //     eventfd_signal(ctx, 1);   /* safe to call in hard IRQ */
	// spin_unlock(&iocache->ev_lock);

    return IRQ_HANDLED;
}

static irqreturn_t iocache_isr_txcomp(int irq, void *data) {
	printk(KERN_DEBUG "TX interrupt received\n");
	// Nothing for now
	return IRQ_HANDLED;
}

static int iocache_parse_irq(struct iocache_device *iocache) {
	struct device *dev = iocache->dev;
	struct device_node *node = dev->of_node;
	int err;

	const char *name = of_node_full_name(node);
	dev_info(dev, "Device Tree node: %s\n", name);

	iocache->rx_irq = irq_of_parse_and_map(node, 0);
	if (!iocache->rx_irq) {
    	dev_err(dev, "Failed to parse RX IRQ from DT\n");
		return -EINVAL;
	}
	
	dev_info(dev, "Requesting RX IRQ %d\n", iocache->rx_irq);
	err = devm_request_irq(dev, iocache->rx_irq, iocache_isr_rx,
			IRQF_SHARED | IRQF_NO_THREAD,
			IOCACHE_NAME, dev);
	if (err) {
		dev_err(dev, "could not obtain rx irq %d\n", iocache->rx_irq);
		return err;
	}

	iocache->txcomp_irq = irq_of_parse_and_map(node, 1);
	if (!iocache->txcomp_irq) {
    	dev_err(dev, "Failed to parse TX_COMP IRQ from DT\n");
		return -EINVAL;
	}
	
	dev_info(dev, "Requesting TX_COMP IRQ %d\n", iocache->txcomp_irq);
	err = devm_request_irq(dev, iocache->txcomp_irq, iocache_isr_txcomp,
			IRQF_SHARED | IRQF_NO_THREAD,
			IOCACHE_NAME, dev);
	if (err) {
		dev_err(dev, "could not obtain tx_comp irq %d\n", iocache->txcomp_irq);
		return err;
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

	spin_lock_init(&iocache->ev_lock);
	u64_stats_init(&iocache->syncp);
	init_waitqueue_head(&iocache->wq);
    atomic_set(&iocache->ready, 0);

	/* Register the misc device */
    iocache->misc_dev.minor = MISC_DYNAMIC_MINOR;
    iocache->misc_dev.name = "iocache-misc";
    iocache->misc_dev.fops = &iocache_fops;
	iocache->misc_dev.parent = dev;

	ret = misc_register(&iocache->misc_dev);
	if (ret) {
		dev_err(dev, "Failed to register misc device");
		free_irq(iocache->rx_irq, dev);
		free_irq(iocache->txcomp_irq, dev);
		return ret;
	}
	else {
		dev_info(dev, "Misc registered :)");
	}

	return 0;
}

static int iocache_remove(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct iocache_device *iocache = platform_get_drvdata(pdev);

    if (!iocache) {
		dev_warn(dev, "Could not find misc device to deregister");
        return 0;
	}

	free_irq(iocache->rx_irq, dev);
	free_irq(iocache->txcomp_irq, dev);

    misc_deregister(&iocache->misc_dev);

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