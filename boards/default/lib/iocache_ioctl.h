#ifndef __IOCACHE_IOCTL_H
#define __IOCACHE_IOCTL_H

#include <linux/types.h>

#define IOCACHE_IOCTL_API_VERSION 0
#define IOCACHE_IOCTL_TYPE 0x88
#define IOCACHE_IOCTL_BASE 0xC0

enum {
    IOCACHE_REGION_TYPE_UNIMPLEMENTED = 0x00000000,
    IOCACHE_REGION_TYPE_CTRL          = 0x00001000
};

/* keep your existing opcodes as-is */
#define IOCACHE_IOCTL_GET_API_VERSION _IO(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 0)

struct iocache_ioctl_device_info {
    __u32 argsz, flags, fw_id, fw_ver, board_id, board_ver, build_date, git_hash, rel_info;
    __u32 num_regions;
    __u32 num_irqs;
};
#define IOCACHE_IOCTL_GET_DEVICE_INFO _IO(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 1)

struct iocache_ioctl_region_info {
    __u32 argsz, flags, index, type, next, child;
    __u64 size, offset;
    __u8  name[32];
};
#define IOCACHE_IOCTL_GET_REGION_INFO _IO(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 2)

/* userspace passes an int (eventfd) */
#define IOCACHE_IOCTL_SET_EVENTFD _IOW(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 3, int)

#define IOCACHE_IOCTL_GET_LAST_IRQ_NS _IOR(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 4, __u64)

#define IOCACHE_IOCTL_WAIT_READY _IOR(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 5, int)

#define IOCACHE_IOCTL_GET_CYCLES _IOR(IOCACHE_IOCTL_TYPE, IOCACHE_IOCTL_BASE + 6, __u64)

#endif /* __IOCACHE_IOCTL_H */
