#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>  
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

#include "iocache_ioctl.h"
#include "iocache_lib.h"
#include "common.h"


static int do_ioctl(int fd, int index, off_t *offset, size_t *size) {
    struct iocache_ioctl_region_info region_info;
    memset(&region_info, 0, sizeof(region_info));
    region_info.argsz = sizeof(region_info);
    region_info.flags = 0;
    region_info.index = index;  // use the parameter

    if (ioctl(fd, IOCACHE_IOCTL_GET_REGION_INFO, &region_info) == -1) {
        perror("IOCACHE_IOCTL_GET_REGION_INFO ioctl failed");
        return -1;
    }

    if (offset) *offset = (off_t)region_info.offset;
    if (size)   *size   = (size_t)region_info.size;

    /* Print safely: name may not be NUL-terminated */
    printf("region index=%d, offset=0x%jx, size=0x%zx, name='%.*s', next=%u, flags=0x%x\n",
           index,
           (uintmax_t)region_info.offset,
           (size_t)region_info.size,
           (int)sizeof region_info.name, region_info.name,
           region_info.next,
           region_info.flags);

    /* Optional: ensure page alignment for mmap */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg > 0 && (region_info.offset & (pg - 1)) != 0) {
        fprintf(stderr, "warning: region offset not page-aligned (0x%jx)\n",
                (uintmax_t)region_info.offset);
    }
    return 0;
}

int iocache_open(char *file, struct iocache_info *iocache) {
    
    iocache->fd = open(file, O_RDWR | O_SYNC);
    if (iocache->fd < 0) {
        perror("open");
        return -1;
    }

    if (do_ioctl(iocache->fd, 0, &iocache->regs_offset, &iocache->regs_size) != 0) {
        close(iocache->fd);
        return -1;
    }

    /* mmap registers and dma memory */
    iocache->regs = (volatile uint8_t *)mmap(NULL, iocache->regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, iocache->fd, MAP_INDEX(0));
    if (iocache->regs == MAP_FAILED) {
        perror("mmap regs failed");
        close(iocache->fd);
        return -1;
    }

    return 0;
}

int iocache_close(struct iocache_info *iocache) {
    munmap((void *) iocache->regs, iocache->regs_size);
    close(iocache->fd);

    return 0;
}