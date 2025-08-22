#include "iocache_ioctl.h"
#include "iocache.h"

#include <linux/mm.h>
#include <asm/set_memory.h>

static int iocache_misc_open(struct inode *inode, struct file *file) {
    struct iocache_device *iocache = container_of(file->private_data, struct iocache_device, misc_dev);
	
	// Sanity check
	if (iocache->magic != MAGIC_CHAR) {
		pr_err("iocache inode 0x%lx magic mismatch 0x%lx\n", 
				inode->i_ino, iocache->magic);
		return -EINVAL;
	} 

    file->private_data = iocache;

    return 0;
}

static int iocache_misc_release(struct inode *inode, struct file *filp) {
    return 0;
}

static long iocache_misc_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct iocache_device *iocache = file->private_data;
	size_t minsz;

	if (cmd == IOCACHE_IOCTL_GET_API_VERSION) {
		// Get API version
		return IOCACHE_IOCTL_API_VERSION;
	} else if (cmd == IOCACHE_IOCTL_GET_DEVICE_INFO) {
		// Get device information
		struct iocache_ioctl_device_info info;

		minsz = offsetofend(struct iocache_ioctl_device_info, num_irqs);

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

	} else if (cmd == IOCACHE_IOCTL_GET_REGION_INFO) {
		// Get region information
		struct iocache_ioctl_region_info info;

		minsz = offsetofend(struct iocache_ioctl_region_info, name);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = 0;
		info.type = IOCACHE_REGION_TYPE_UNIMPLEMENTED;
		info.next = 0;
		info.child = 0;
		info.size = 0;
		info.offset = ((u64)info.index) << 40;
		info.name[0] = 0;

		switch (info.index) {
		case 0:
			info.type = IOCACHE_REGION_TYPE_CTRL;
			info.next = 1;
			info.child = 0;
			info.size = iocache->hw_regs_control_size;
			info.offset = ((u64)info.index) << 40;
			strlcpy(info.name, "ctrl", sizeof(info.name));
			break;
		default:
			return -EINVAL;
		}
		return copy_to_user((void __user *)arg, &info, minsz) ? -EFAULT : 0;
	}
	return -EINVAL;
}

static int iocache_misc_mmap(struct file *file, struct vm_area_struct *vma) {
    struct iocache_device *iocache = file->private_data;

	int index;
	u64 pgoff, req_len, req_start;

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
        if (req_start + req_len > iocache->hw_regs_control_size)
			return -EINVAL;

		return io_remap_pfn_range(vma, vma->vm_start,
				(iocache->hw_regs_control_phys >> PAGE_SHIFT) + pgoff,
				req_len, pgprot_noncached(vma->vm_page_prot));
    }
    default:
        dev_err(iocache->dev, "%s: unknown region index=%d pgoff=0x%lx\n",
                __func__, index, vma->vm_pgoff);
        return -EINVAL;
    }
}
