#include "accnet_ioctl.h"
#include "accnet.h"

#include <linux/mm.h>
#include <asm/set_memory.h>

// File open operation
static int accnet_misc_open(struct inode *inode, struct file *file) {

    struct accnet_device *nic = container_of(file->private_data, struct accnet_device, misc_dev);
	
	// Sanity check
	if (nic->magic != MAGIC_CHAR) {
		pr_err("nic inode 0x%lx magic mismatch 0x%lx\n", 
				inode->i_ino, nic->magic);
		return -EINVAL;
	} 

    file->private_data = nic;

    return 0;
}

// File release operation
static int accnet_misc_release(struct inode *inode, struct file *filp) {
    return 0;
}

static long accnet_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct accnet_device *nic = file->private_data;
	size_t minsz;

	if (cmd == ACCNET_IOCTL_GET_API_VERSION) {
		// Get API version
		return ACCNET_IOCTL_API_VERSION;
	} else if (cmd == ACCNET_IOCTL_GET_DEVICE_INFO) {
		// Get device information
		struct accnet_ioctl_device_info info;

		minsz = offsetofend(struct accnet_ioctl_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = 0;
		info.fw_id = 11;
		info.fw_ver = 12;
		info.board_id = 13;
		info.board_ver =14;
		info.build_date = 15;
		info.git_hash = 16;
		info.rel_info = 17;
		info.num_regions = 1;
		info.num_irqs = 0;

		return copy_to_user((void __user *)arg, &info, minsz) ? -EFAULT : 0;

	} else if (cmd == ACCNET_IOCTL_GET_REGION_INFO) {
		// Get region information
		struct accnet_ioctl_region_info info;

		minsz = offsetofend(struct accnet_ioctl_region_info, name);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = 0;
		info.type = ACCNET_REGION_TYPE_UNIMPLEMENTED;
		info.next = 0;
		info.child = 0;
		info.size = 0;
		info.offset = ((u64)info.index) << 40;
		info.name[0] = 0;

		switch (info.index) {
		case 0:
			info.type = ACCNET_REGION_TYPE_NIC_CTRL;
			info.next = 1;
			info.child = 0;
			info.size = nic->hw_regs_control_size;
			info.offset = ((u64)info.index) << 40;
			strlcpy(info.name, "ctrl", sizeof(info.name));
			break;
        case 1:
			info.type = ACCNET_REGION_TYPE_NIC_CTRL;
			info.next = 2;
			info.child = 0;
			info.size = nic->hw_regs_udp_tx_size;
			info.offset = ((u64)info.index) << 40;
			strlcpy(info.name, "udp-tx", sizeof(info.name));
			break;
        case 2:
			info.type = ACCNET_REGION_TYPE_NIC_CTRL;
			info.next = 3;
			info.child = 0;
			info.size = nic->hw_regs_udp_rx_size;
			info.offset = ((u64)info.index) << 40;
			strlcpy(info.name, "udp-rx", sizeof(info.name));
			break;
		default:
			return -EINVAL;
		}

		return copy_to_user((void __user *)arg, &info, minsz) ? -EFAULT : 0;

	}

	return -EINVAL;
}

// mmap operation to map DMA buffer and NIC registers to userspace
static int accnet_misc_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct accnet_device *nic = filp->private_data;

	int index;
	u64 pgoff, req_len, req_start;

	unsigned long phys = 0;
	unsigned long psize;

	index = vma->vm_pgoff >> (40 - PAGE_SHIFT);
	req_len = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff & ((1U << (40 - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;

	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;

	pr_info("mmap: pgoff=%#lx size=%#lx\n", vma->vm_pgoff, vma->vm_end - vma->vm_start);

    switch (index) {
    case 0: { /* control MMIO */
        if (req_start + req_len > nic->hw_regs_control_size)
			return -EINVAL;

		return io_remap_pfn_range(vma, vma->vm_start,
				(nic->hw_regs_control_phys >> PAGE_SHIFT) + pgoff,
				req_len, pgprot_noncached(vma->vm_page_prot));
    }
    case 1: { /* UDP TX regs MMIO */
        if (req_start + req_len > nic->hw_regs_udp_tx_size)
			return -EINVAL;

		return io_remap_pfn_range(vma, vma->vm_start,
				(nic->hw_regs_udp_tx_phys >> PAGE_SHIFT) + pgoff,
				req_len, pgprot_noncached(vma->vm_page_prot));
    }
    case 2: { /* UDP RX regs MMIO */
        if (req_start + req_len > nic->hw_regs_udp_rx_size)
			return -EINVAL;

		return io_remap_pfn_range(vma, vma->vm_start,
				(nic->hw_regs_udp_rx_phys >> PAGE_SHIFT) + pgoff,
				req_len, pgprot_noncached(vma->vm_page_prot));
    }
    case 3: { /* UDP TX DMA buffer (coherent) */
		pr_info("udp_tx: cpu=%p dma=%p len=%#zx\n",
            nic->dma_region_udp_tx, &nic->dma_region_addr_udp_tx, nic->dma_region_len_udp_tx);

        phys = virt_to_phys(nic->dma_region_udp_tx);  // Convert virtual to physical address
        psize = nic->dma_region_len_udp_tx;

		if (req_len > psize)
		{
			printk("*** 2: req_len = %llu,  psize = %lu *** \n", req_len, psize);
			return -EINVAL;
		}

		vm_flags_mod(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP, 0);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vma->vm_pgoff = 0;
		
		return dma_mmap_coherent(nic->dev, vma, nic->dma_region_udp_tx, nic->dma_region_addr_udp_tx, req_len);
    }
    case 4: { /* UDP RX DMA buffer (coherent) */
		pr_info("udp_rx: cpu=%p dma=%p len=%#zx\n",
            nic->dma_region_udp_rx, &nic->dma_region_addr_udp_rx, nic->dma_region_len_udp_rx);

        phys = virt_to_phys(nic->dma_region_udp_rx);  // Convert virtual to physical address
        psize = nic->dma_region_len_udp_rx;

		if (req_len > psize)
		{
			printk("*** 3: req_len = %llu,  psize = %lu *** \n", req_len, psize);
			return -EINVAL;
		}

		vm_flags_mod(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP, 0);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vma->vm_pgoff = 0;
		
		return dma_mmap_coherent(nic->dev, vma, nic->dma_region_udp_rx, nic->dma_region_addr_udp_rx, req_len);
    }
    default:
        dev_err(nic->dev, "%s: unknown region index=%d pgoff=0x%lx\n",
                __func__, index, vma->vm_pgoff);
        return -EINVAL;
    }
}
