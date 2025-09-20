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

    if (!tsk || (tsk && task_is_running(tsk))) {
        return HRTIMER_NORESTART;
	}
	
	// int cpu = smp_processor_id();
	// printk(KERN_INFO "Timer hit, cacheID=%d, cpu=%d\n", tsk->iocache_id, cpu);

	if (!tsk->iocache_id_valid) {
		printk(KERN_WARNING "Timer without a row!\n");
	}

    /* Wake it */
	iowrite8 (0, 	REG(tsk->iocache_iomem, IOCACHE_REG_RX_SUSPENDED(tsk->iocache_id)));
	mmiowb();

	set_tsk_need_resched(current);  

    return HRTIMER_NORESTART;
}

static enum hrtimer_restart iocache_wakeup_timer(struct hrtimer *t)
{
    struct task_struct *tsk = container_of(t, struct task_struct, wakeup_hrtimer);

    if (!tsk) {
        return HRTIMER_NORESTART;
	}
	
	wake_up_process(tsk);
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

static int iocache_misc_release(struct inode *inode, struct file *file)
{
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
    } else if (cmd == IOCACHE_IOCTL_WAIT_TIMEOUT) {
		/* Prepare to sleep (interruptible) */
		int row = READ_ONCE(current->iocache_id);

		iowrite8 (1, 	REG(iocache->iomem, IOCACHE_REG_RX_SUSPENDED(row)));
		mmiowb();

		// printk(KERN_INFO "starting wait: id=%d\n", row);

		/* Start a pinned hrtimer for the timeout on this CPU */
    	hrtimer_start(&current->to_hrtimer, current->to_period, HRTIMER_MODE_REL_PINNED_HARD);
		/* Sleep;
		 * Timeout will wake us via wake_up_process()
		 */
		schedule();
		hrtimer_cancel(&current->to_hrtimer);

		// printk(KERN_INFO "stopping wait: id=%d\n", row);

		iowrite8 (0, 	REG(iocache->iomem, IOCACHE_REG_RX_SUSPENDED(row)));
		mmiowb();


		// iocache->syscall_time = ktime_get_mono_fast_ns();

		return 0;
	} else if (cmd == IOCACHE_IOCTL_RESERVE_RING) {
		int row;
		unsigned long flags;

		spin_lock_irqsave(&iocache->row_alloc_lock, flags);

		row = ioread32(	REG(iocache->iomem, IOCACHE_REG_ALLOC_FIRST_EMPTY));
		uint8_t is_enabled = ioread8( REG(iocache->iomem, IOCACHE_REG_ENABLED(row) )); 

		if (is_enabled) {
			spin_unlock_irqrestore(&iocache->row_alloc_lock, flags);
			printk(KERN_WARNING "No available rows in IOCache\n");
			return -ENOMEM;
		}
		
		iowrite8 (1, 	REG(iocache->iomem, IOCACHE_REG_ENABLED(row)));
		smp_wmb();

		WRITE_ONCE(current->iocache_id, 		row);
		WRITE_ONCE(current->iocache_id_valid, 	true);

		spin_unlock_irqrestore(&iocache->row_alloc_lock, flags);

        if (copy_to_user((void __user *)arg, &row, sizeof(row)))
            return -EFAULT;

        return 0;
	} else if (cmd == IOCACHE_IOCTL_FREE_RING) {
		int row;
		unsigned long flags;

		spin_lock_irqsave(&iocache->row_alloc_lock, flags);

		if (READ_ONCE(current->iocache_id_valid)) {
			row = READ_ONCE(current->iocache_id);
			iowrite8 (0, 							REG(iocache->iomem, IOCACHE_REG_ENABLED(row)));
			smp_wmb();

			WRITE_ONCE(current->iocache_id_valid, 	false);
			WRITE_ONCE(current->iocache_id, 		0);
		}

		spin_unlock_irqrestore(&iocache->row_alloc_lock, flags);

        return 0;
	} else if (cmd == IOCACHE_IOCTL_START_SCHEDULER) {
		int row;
		int cpu;
		unsigned long flags;

		if (!READ_ONCE(current->iocache_id_valid)) {
			printk(KERN_WARNING "no row initialized\n");
			return -EPERM;
		}

		/* Keep this thread on the same CPU while we program it */
		migrate_disable();
		cpu = smp_processor_id();

		spin_lock_irqsave(&iocache->sched_lock, flags);

		WRITE_ONCE(current->iocache_iomem, iocache->iomem);
		row = READ_ONCE(current->iocache_id);
		
		iowrite32(cpu, 							REG(iocache->iomem, IOCACHE_REG_PROC_CPU(row)));
		iowrite64((u64) (uintptr_t) current, 	REG(iocache->iomem, IOCACHE_REG_PROC_PTR(row)));

		/* diactivate thread; order matters */
		deactivate_process_iocache(current);
		set_current_state(TASK_INTERRUPTIBLE);

		spin_unlock_irqrestore(&iocache->sched_lock, flags);

		hrtimer_init(&current->to_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
		current->to_hrtimer.function = iocache_timeout_cb;
		current->to_period = ktime_set(1, 0); // 1s 

        return 0;
	} else if (cmd == IOCACHE_IOCTL_STOP_SCHEDULER) {
		unsigned long flags;
		int row = READ_ONCE(current->iocache_id);

		if (!READ_ONCE(current->iocache_id_valid)) {
			printk(KERN_WARNING "no row initialized\n");
			return -EPERM;
		}

		hrtimer_cancel(&current->to_hrtimer);

		hrtimer_init(&current->wakeup_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
		current->wakeup_hrtimer.function = iocache_wakeup_timer;
		current->wakeup_period = ktime_set(0, 2e8); // 200ms 

		spin_lock_irqsave(&iocache->sched_lock, flags);
		wake_up_process_iocache(current);
		set_current_state(TASK_RUNNING);
		
		iowrite64(0, 							REG(iocache->iomem, IOCACHE_REG_PROC_PTR(row)));
		iowrite32(0, 							REG(iocache->iomem, IOCACHE_REG_PROC_CPU(row)));

		// hrtimer_start(&current->wakeup_hrtimer, current->wakeup_period, HRTIMER_MODE_REL_PINNED_HARD);

		spin_unlock_irqrestore(&iocache->sched_lock, flags);

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
