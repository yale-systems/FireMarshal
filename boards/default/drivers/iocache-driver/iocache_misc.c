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
    struct iocache_device *dev = container_of(t, struct iocache_device, to_hrtimer);
    struct task_struct *tsk;

    /* Ensure we only proceed if the arming flag is still set. */
    if (!smp_load_acquire(&dev->armed)) 
        return HRTIMER_NORESTART;

    tsk = READ_ONCE(dev->wait_task);
    if (!tsk || (tsk && task_is_running(tsk)))
        return HRTIMER_NORESTART;

    /* Wake it */
	WRITE_ONCE(tsk->__state, TASK_RUNNING);
	set_tsk_need_resched(current);  

    return HRTIMER_NORESTART;
}

static int iocache_misc_open(struct inode *inode, struct file *file) {
    struct iocache_device *iocache = container_of(file->private_data, struct iocache_device, misc_dev);

	// Sanity check
	if (iocache->magic != MAGIC_CHAR) {
		pr_err("iocache inode 0x%lx magic mismatch 0x%lx\n", 
				inode->i_ino, iocache->magic);
		return -EINVAL;
	} 

    file->private_data = iocache;

	/* Publish waiter then arm ISR wake; order matters */
	sched_force_next_local(current);
	WRITE_ONCE(iocache->wait_task, current);
	smp_wmb();                      // publish tsk before arming

	hrtimer_init(&iocache->to_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	iocache->to_hrtimer.function = iocache_timeout_cb;
	iocache->to_period = ktime_set(1, 0); // 1 second 

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

	wake_up_process_iocache(current);
	WRITE_ONCE(iocache->wait_task, NULL);
	schedule();

	// iocache_print_cpu_util();
	// printk(KERN_INFO "\nIOCACHE Released successfull\n");
	sched_force_next_local(NULL);

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
		/* Fast path: event already present */
		if (READ_ONCE(iocache->ready)) {
			WRITE_ONCE(iocache->ready, 0);
			// iocache->syscall_time = ktime_get_mono_fast_ns();
			return 0;
		}

		/* Prepare to sleep (interruptible) */
		set_current_state(TASK_INTERRUPTIBLE);

		WRITE_ONCE(iocache->armed, 1);

		/* Recheck after arming to avoid lost wake between checks */
		if (READ_ONCE(iocache->ready)) {
			__set_current_state(TASK_RUNNING);
			WRITE_ONCE(iocache->armed, 0);
			WRITE_ONCE(iocache->ready, 0);
			// iocache->syscall_time = ktime_get_mono_fast_ns();
			return 0;
		}

		/* Start a pinned hrtimer for the timeout on this CPU */
    	hrtimer_start(&iocache->to_hrtimer, iocache->to_period, HRTIMER_MODE_REL_PINNED);
		/* Sleep;
		 * Timeout will wake us via wake_up_process()
		 * Device interrupt will set current to TASK_RUNNING and run this
		 */
		schedule();
		__set_current_state(TASK_RUNNING);
		hrtimer_cancel(&iocache->to_hrtimer);

		// iocache->syscall_time = ktime_get_mono_fast_ns();


		/* Running again: clean up publication on every path */
		WRITE_ONCE(iocache->armed, 0);
		WRITE_ONCE(iocache->ready, 0);
		return 0;
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
        dev_err(iocache->dev, "%s: unknown region index=%d pgoff=0x%lx\n",
                __func__, index, vma->vm_pgoff);
        return -EINVAL;
    }
}
