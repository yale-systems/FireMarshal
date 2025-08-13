# ---- config ----
CROSS   ?= riscv64-unknown-linux-gnu-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# Tune these if your rootfs expects something else
ARCH    ?= rv64gc
ABI     ?= lp64d

# Optional: set SYSROOT=/path/to/sysroot if you have one
SYSROOT ?=

CFLAGS  ?= -O2 -g -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith
CFLAGS  += -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS :=
LDFLAGS  =

ifneq ($(strip $(SYSROOT)),)
CPPFLAGS += --sysroot=$(SYSROOT)
LDFLAGS  += --sysroot=$(SYSROOT)
endif

TARGET = mynic
SRCS   = mynic.c
HDRS   = mynic.h accnet_ioctl.h
OBJS   = $(SRCS:.c=.o)
DEPS   = $(OBJS:.o=.d)

# ---- rules ----
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -march=$(ARCH) -mabi=$(ABI) -MMD -MP -c $< -o $@

strip: $(TARGET)
	$(STRIP) $<

# Build a static binary (glibc may fail; works best with musl)
static:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -static" LDFLAGS="$(LDFLAGS) -static"

clean:
	$(RM) $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
