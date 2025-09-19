#include <linux/errno.h>
#include <linux/types.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/io.h>

#include "iocache.h"

#define PLIC_PRIO_OFF(hwirq)   (0x00000000u + 4u * (hwirq))
#define MAX_PRIORITY           0xFFFFFFFF

/* Provided by our patched PLIC file */
int plic_register_source_handler(int hwirq,
                                 irqreturn_t (*fn)(int, void *),
                                 void *data);

/* Provided by our patched PLIC file */
int plic_unregister_source_handler(int hwirq);

								 
static unsigned int get_hwirq(unsigned int virq)
{
    struct irq_data *d = irq_get_irq_data(virq);
    return d ? (unsigned int)irqd_to_hwirq(d) : 0;
}

static void plic_set_src_prio(struct iocache_device *iocache, unsigned int hwirq, u32 want_prio)
{
    void __iomem *prio = iocache->plic_base + PLIC_PRIO_OFF(hwirq);
    writel(want_prio, prio);
    /* Read back to learn the real max (hardware may clamp) */
    u32 got = readl(prio);
    pr_info("PLIC hwirq %u priority set to %u\n", hwirq, got);
}

static int map_plic_base(struct iocache_device *iocache)
{
    struct device_node *np =
        of_find_compatible_node(NULL, NULL, "riscv,plic0");
    if (!np)
        np = of_find_compatible_node(NULL, NULL, "sifive,plic-1.0.0");
    if (!np) return -ENODEV;

    iocache->plic_base = of_iomap(np, 0);  // region 0 is the whole PLIC
    of_node_put(np);
    return iocache->plic_base ? 0 : -ENOMEM;
}

static int plic_set_prio(struct iocache_device *iocache) {
    int ret;
	unsigned int hwirq;

	/* Map the PLIC MMIO region once */
	ret = map_plic_base(iocache);
	if (ret) {
		dev_err(iocache->dev, "failed to map PLIC base: %d\n", ret);
		return ret;
	}

	for (uint32_t i = 0; i < NUM_CPUS; i++) {
		/* RX IRQ: resolve to hwirq and set priority */
		hwirq = get_hwirq(iocache->rx_irq[i]);
		if (!hwirq) {
			dev_err(iocache->dev, "could not resolve hwirq for RX irq %d\n",
				iocache->rx_irq[i]);
			return ret;
		} else {
			iocache->rx_hwirq[i] = hwirq;
			plic_set_src_prio(iocache, hwirq, MAX_PRIORITY);
		}

		/* TX completion IRQ: resolve to hwirq and set priority */
		hwirq = get_hwirq(iocache->txcomp_irq[i]);
		if (!hwirq) {
			dev_err(iocache->dev, "could not resolve hwirq for TXCOMP irq %d\n",
					iocache->txcomp_irq[i]);
			return ret;
		} else {
			iocache->txcomp_hwirq[i] = hwirq;
			plic_set_src_prio(iocache, hwirq, MAX_PRIORITY);
		}
	}

    return 0;
}

static int plic_register_fast_path(
	struct iocache_device *iocache, 
	irqreturn_t (*fn_rx)(int, void *), 
	irqreturn_t (*fn_txcomp)(int, void *)
) {
    int ret;
	unsigned int hwirq;

	for (uint32_t i = 0; i < NUM_CPUS; i++) {
		/* RX IRQ: resolve to hwirq and Register PLIC bypass */
		hwirq = get_hwirq(iocache->rx_irq[i]);
		if (!hwirq) {
			dev_err(iocache->dev, "could not resolve hwirq for RX irq %d\n",
					iocache->rx_irq[i]);
			return ret;
		} else {
			iocache->rx_hwirq[i] = hwirq;
			ret = plic_register_source_handler(hwirq, fn_rx, iocache);
			if (ret) 
				return ret;
		}
	
		/* TX completion IRQ: resolve to hwirq and Register PLIC bypass */
		hwirq = get_hwirq(iocache->txcomp_irq[i]);
		if (!hwirq) {
			dev_err(iocache->dev, "could not resolve hwirq for TXCOMP irq %d\n",
					iocache->txcomp_irq[i]);
			return ret;
		} else {
			iocache->txcomp_hwirq[i] = hwirq;
			ret = plic_register_source_handler(hwirq, fn_txcomp, iocache);
			if (ret) 
				return ret;
		}
	}

    return 0;
}

static int plic_unregister_fast_path(struct iocache_device *iocache) {
	for (uint32_t i = 0; i < NUM_CPUS; i++) {
		plic_unregister_source_handler(iocache->rx_hwirq[i]);
		plic_unregister_source_handler(iocache->txcomp_hwirq[i]);
	}
	return 0;
}