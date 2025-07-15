ifneq ($(KERNELRELEASE),)

obj-m += accnet.o

else

# The default assumes you cloned this as part of firesim-software (FireMarshal)
# LINUXSRC=../../../../riscv-linux
LINUXSRC=../../linux

KMAKE=make -C $(LINUXSRC) ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- M=$(PWD)

accnet.ko: accnet.c
	$(KMAKE)

clean:
	$(KMAKE) clean

endif
