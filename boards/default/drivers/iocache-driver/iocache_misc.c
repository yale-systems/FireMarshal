#include "iocache_ioctl.h"
#include "iocache.h"

#include <linux/mm.h>
#include <asm/set_memory.h>
#include <linux/eventfd.h>
#include <linux/math64.h> 
#include <linux/sched/clock.h>
#include <linux/sched.h>

#include <linux/preempt.h>
#include <linux/smp.h> 
#include <linux/console.h>

static enum hrtimer_restart iocache_timeout_cb(struct hrtimer *t)
{
    struct task_struct *tsk = container_of(t, struct task_struct, to_hrtimer);

    if (!tsk || !tsk->is_iocache_managed) {
        return HRTIMER_NORESTART;
	}

	int suspended = ioread8(REG(tsk->iocache_iomem, IOCACHE_REG_RX_SUSPENDED(tsk->iocache_id)));
	if (!suspended) {
		return HRTIMER_NORESTART;
	}
	
	// int cpu = smp_processor_id();
	// printk(KERN_INFO "Timer hit, cacheID=%d, cpu=%d\n", tsk->iocache_id, cpu);

    /* Wake it */
	iowrite8 (0, 	REG(tsk->iocache_iomem, IOCACHE_REG_RX_SUSPENDED(tsk->iocache_id)));
	// iowrite8 (0, 	REG(tsk->iocache_iomem, IOCACHE_REG_TXCOMP_SUSPENDED(tsk->iocache_id)));
	mmiowb();

	// WRITE_ONCE(tsk->__state, TASK_RUNNING);
	set_tsk_need_resched(current);  


    return HRTIMER_NORESTART;
}

static int iocache_misc_open(struct inode *inode, struct file *file) {
	// printk(KERN_INFO "Openning iocache-misc\n");
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

// static void iocache_print_cpu_util(void) {
// 	u64 usage, dur, pct_x10;

// 	rcu_read_lock();
// 	usage = READ_ONCE(current->my_oncpu_total_ns);
// 	dur = READ_ONCE(current->my_oncpu_wall_ns);
// 	rcu_read_unlock();

// 	pct_x10 = dur ? mul_u64_u32_div(usage, 1000, dur) : 0;  /* = percent * 10 */

// 	console_lock(); 
// 	pr_info("\n*** Process utilization: %llu.%01llu %%\n",
// 		(unsigned long long)(pct_x10 / 10),
// 		(unsigned long long)(pct_x10 % 10));
// 	console_unlock();
// } 

static int iocache_misc_release(struct inode *inode, struct file *filp)
{
	// printk(KERN_INFO "Releasing iocache-misc: id=%d\n", current->iocache_id);

    struct iocache_device *iocache = filp->private_data;

    if (iocache) {
        struct eventfd_ctx *old = NULL;
        spin_lock(&iocache->ev_lock);
        old = iocache->ev_ctx;
        iocache->ev_ctx = NULL;
        spin_unlock(&iocache->ev_lock);
        if (old) eventfd_ctx_put(old);
    }

	/* 
	 * We need it to end a process correctly by re-adding it to ready queues. 
	 * We call wake_up_process_iocache() that will automatically add our process to linux queues 
	 * and we will unset force_next_local.
	 */
	// wake_up_process_iocache(current);
	// sched_force_next_local(NULL);
	// smp_wmb();
	// schedule();

	// wake_up_process(current);
	// wake_up_process_iocache(current);
	// __set_current_state(TASK_IOCACHE_SPECIAL);
	// schedule();

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
	} else if (cmd == IOCACHE_IOCTL_SET_EVENTFD) {
        int efd;
        struct eventfd_ctx *ctx, *old = NULL;

        if (copy_from_user(&efd, (void __user *)arg, sizeof(efd)))
            return -EFAULT;

        /* allow -1 to clear */
        if (efd == -1) {
            spin_lock(&iocache->ev_lock);
            old = iocache->ev_ctx;
            iocache->ev_ctx = NULL;
            spin_unlock(&iocache->ev_lock);
            if (old) eventfd_ctx_put(old);
            return 0;
        }

        ctx = eventfd_ctx_fdget(efd);   /* takes a ref */
        if (IS_ERR(ctx))
            return PTR_ERR(ctx);

        spin_lock(&iocache->ev_lock);
        old = iocache->ev_ctx;
        iocache->ev_ctx = ctx;
        spin_unlock(&iocache->ev_lock);

        if (old) eventfd_ctx_put(old);
        return 0;
    } else if (cmd == IOCACHE_IOCTL_GET_KTIMES) {
		u64 val[4] = {iocache->entry_ktime, iocache->claim_ktime, iocache->isr_ktime, iocache->syscall_time};
		
        if (copy_to_user((void __user *)arg, &val, sizeof(val)))
            return -EFAULT;
        return 0;
    } else if (cmd == IOCACHE_IOCTL_GET_PROC_UTIL) {
		// u64 usage;

		// rcu_read_lock();
		// usage = READ_ONCE(current->my_oncpu_total_ns);
		// rcu_read_unlock();

		u64 val[3] = {0};

        if (copy_to_user((void __user *)arg, &val, sizeof(val)))
            return -EFAULT;
        return 0;
    } else if (cmd == IOCACHE_IOCTL_WAIT_READY) {
		/* Prepare to sleep (interruptible) */
		int row = READ_ONCE(current->iocache_id);
		set_current_state(TASK_INTERRUPTIBLE);

		iowrite8 (1, 	REG(iocache->iomem, IOCACHE_REG_RX_SUSPENDED(row)));
		// iowrite8 (1, 	REG(iocache->iomem, IOCACHE_REG_TXCOMP_SUSPENDED(row)));
		mmiowb();

		// printk(KERN_INFO "starting wait: id=%d\n", row);

		/* Start a pinned hrtimer for the timeout on this CPU */
    	hrtimer_start(&current->to_hrtimer, current->to_period, HRTIMER_MODE_REL_PINNED);
		/* Sleep;
		 * Timeout will wake us via wake_up_process()
		 * Device interrupt will set current to TASK_RUNNING and run this
		 */
		schedule();
		__set_current_state(TASK_RUNNING);
		hrtimer_cancel(&current->to_hrtimer);

		// printk(KERN_INFO "stopping wait: id=%d\n", row);

		iowrite8 (0, 	REG(iocache->iomem, IOCACHE_REG_RX_SUSPENDED(row)));
		// iowrite8 (0, 	REG(iocache->iomem, IOCACHE_REG_TXCOMP_SUSPENDED(row)));
		mmiowb();

		// iocache->syscall_time = ktime_get_mono_fast_ns();

		return 0;
	} else if (cmd == IOCACHE_IOCTL_GET_AVAIL_RING) {
		int row = current->iocache_id;

        if (copy_to_user((void __user *)arg, &row, sizeof(row)))
            return -EFAULT;
        return 0;
	} else if (cmd == IOCACHE_IOCTL_RESERVE_RING) {
		int row;

		if (copy_from_user(&row, (void __user *)arg, sizeof(row)))
            return -EFAULT;

		iowrite8 (1, 	REG(iocache->iomem, IOCACHE_REG_ENABLED(row)));
		mmiowb();
		WRITE_ONCE(current->iocache_id, row);

        if (copy_to_user((void __user *)arg, &row, sizeof(row)))
            return -EFAULT;
        return 0;
	} else if (cmd == IOCACHE_IOCTL_FREE_RING) {
		int row = current->iocache_id;

		spin_lock(&iocache->ring_alloc_lock);

		iowrite8 (0, 	REG(iocache->iomem, IOCACHE_REG_ENABLED(row)));

		spin_unlock(&iocache->ring_alloc_lock);

        return 0;
	} else if (cmd == IOCACHE_IOCTL_RUN_SCHEDULER) {
		int row;
		int cpu;

		migrate_disable();
		cpu = smp_processor_id();

		row = current->iocache_id;

		WRITE_ONCE(current->iocache_iomem, iocache->iomem);

		// printk(KERN_INFO "Starting Scheduler: id=%d, cpu=%d\n", row, cpu);

		/* diactivate thread; order matters */
		sched_force_next_local(current);

		hrtimer_init(&current->to_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		current->to_hrtimer.function = iocache_timeout_cb;
		current->to_period = ktime_set(1, 0); // 1s 

        return 0;
	} else if (cmd == IOCACHE_IOCTL_STOP_SCHEDULER) {
		int row = current->iocache_id;
		
		// int cpu = smp_processor_id();
		// printk(KERN_INFO "Stopping Scheduler: id=%d, cpu=%d\n", row, cpu);

		// schedule();
		
		wake_up_process_iocache(current);
		
		// sched_force_next_local(NULL);
		// set_tsk_need_resched(current);  
		// set_current_state(TASK_RUNNING);
		// schedule();

		hrtimer_cancel(&current->to_hrtimer);
        return 0;
	}

	return -EINVAL;
}

static int iocache_misc_mmap(struct file *file, struct vm_area_struct *vma) {
    struct iocache_device *iocache = file->private_data;

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

	// pr_info("mmap: pgoff=%#lx size=%#lx\n", vma->vm_pgoff, vma->vm_end - vma->vm_start);

    switch (index) {
    case 0: { /* control MMIO */
        if (req_start + req_len > iocache->hw_regs_control_size)
			return -EINVAL;

		return io_remap_pfn_range(vma, vma->vm_start,
				(iocache->hw_regs_control_phys >> PAGE_SHIFT) + pgoff,
				req_len, pgprot_noncached(vma->vm_page_prot));
    }
    default:
		break;
    }

	if (index >= 1 + 2 * IOCACHE_CACHE_ENTRY_COUNT || index <= 0) {
		dev_err(iocache->dev, "%s: unknown region index=%d pgoff=0x%lx\n",
                __func__, index, vma->vm_pgoff);
        return -EINVAL;
	}

	if (index % 2 == 0) {
		// We should map RX
		int ring_idx = (index - 2) / 2;

		phys = virt_to_phys(iocache->dma_region_udp_rx[ring_idx]);  // Convert virtual to physical address
        psize = iocache->dma_region_len_udp_rx[ring_idx];

		if (req_len > psize)
		{
			printk("*** 3: req_len = %llu,  psize = %lu *** \n", req_len, psize);
			return -EINVAL;
		}

		vm_flags_mod(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP, 0);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vma->vm_pgoff = 0;
		
		return dma_mmap_coherent(iocache->dev, vma, 
									iocache->dma_region_udp_rx[ring_idx], 
									iocache->dma_region_addr_udp_rx[ring_idx], 
									req_len);
	}
	else {
		// MMAP TX
		int ring_idx = (index - 1) / 2;

		phys = virt_to_phys(iocache->dma_region_udp_tx[ring_idx]);  // Convert virtual to physical address
        psize = iocache->dma_region_len_udp_tx[ring_idx];

		if (req_len > psize)
		{
			printk("*** 2: req_len = %llu,  psize = %lu *** \n", req_len, psize);
			return -EINVAL;
		}

		vm_flags_mod(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP, 0);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vma->vm_pgoff = 0;
		
		return dma_mmap_coherent(iocache->dev, vma, 
									iocache->dma_region_udp_tx[ring_idx], 
									iocache->dma_region_addr_udp_tx[ring_idx], 
									req_len);
	}
}
