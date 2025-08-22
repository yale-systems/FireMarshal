ifneq ($(KERNELRELEASE),)

obj-m += iocache.o

else

# The default assumes you cloned this as part of firesim-software (FireMarshal)
# LINUXSRC=../../../../riscv-linux
LINUXSRC=../../linux

KMAKE=make -C $(LINUXSRC) ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- M=$(PWD)

iocache.ko: iocache.c iocache.h iocache_misc.c iocache_ioctl.h
	$(KMAKE)

clean:
	$(KMAKE) clean

endif
