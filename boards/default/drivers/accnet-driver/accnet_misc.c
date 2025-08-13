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

    const unsigned idx_bits = 40 - PAGE_SHIFT;
    int index;
    unsigned long off_pages, req_len_ul;
    u64 req_len, req_start;  /* keep wide math for overflow checks */

    if (vma->vm_end < vma->vm_start)
        return -EINVAL;

    if (!(vma->vm_flags & VM_SHARED))
        return -EINVAL;

    index     = vma->vm_pgoff >> idx_bits;
    off_pages = vma->vm_pgoff & ((1UL << idx_bits) - 1);   /* note 1UL */
    req_len   = (u64)vma->vm_end - (u64)vma->vm_start;
    req_len_ul = vma->vm_end - vma->vm_start;              /* for APIs needing unsigned long */
    req_start = (u64)off_pages << PAGE_SHIFT;

    switch (index) {
    case 0: { /* control MMIO */
        if (req_start + req_len < req_start || req_start + req_len > nic->hw_regs_control_size)
            return -EINVAL;

        return vm_iomap_memory(vma, nic->hw_regs_control_phys + req_start, req_len_ul);
    }
    case 1: { /* UDP TX regs MMIO */
        if (req_start + req_len < req_start || req_start + req_len > nic->hw_regs_udp_tx_size)
            return -EINVAL;

        return vm_iomap_memory(vma, nic->hw_regs_udp_tx_phys + req_start, req_len_ul);
    }
    case 2: { /* UDP RX regs MMIO */
        if (req_start + req_len < req_start || req_start + req_len > nic->hw_regs_udp_rx_size)
            return -EINVAL;

        return vm_iomap_memory(vma, nic->hw_regs_udp_rx_phys + req_start, req_len_ul);
    }
    case 3: { /* UDP TX DMA buffer (coherent) */
        dma_addr_t dma = nic->dma_region_addr_udp_tx;
        size_t     size = nic->dma_region_len_udp_tx;

        if (req_start + req_len < req_start || req_start + req_len > size)
            return -EINVAL;

        // vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP; /* no VM_IO here */
        /* dma_mmap_coherent wants PFN of dma handle + offset */
        vma->vm_pgoff = (dma >> PAGE_SHIFT) + off_pages;

        return dma_mmap_coherent(nic->dev, vma,
                                 nic->dma_region_udp_tx,   /* cpu addr */
                                 dma,                      /* dma handle */
                                 req_len_ul);
    }
    case 4: { /* UDP RX DMA buffer (coherent) */
        dma_addr_t dma = nic->dma_region_addr_udp_rx;
        size_t     size = nic->dma_region_len_udp_rx;

        if (req_start + req_len < req_start || req_start + req_len > size)
            return -EINVAL;

        // vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
        vma->vm_pgoff = (dma >> PAGE_SHIFT) + off_pages;

        return dma_mmap_coherent(nic->dev, vma,
                                 nic->dma_region_udp_rx,
                                 dma,
                                 req_len_ul);
    }
    default:
        dev_err(nic->dev, "%s: unknown region index=%d pgoff=0x%lx\n",
                __func__, index, vma->vm_pgoff);
        return -EINVAL;
    }
}
