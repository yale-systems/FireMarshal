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
LDFLAGS  = -D_GNU_SOURCE
LDLIBS   = -pthread

ifneq ($(strip $(SYSROOT)),)
CPPFLAGS += --sysroot=$(SYSROOT)
LDFLAGS  += --sysroot=$(SYSROOT)
endif

# ---- apps and sources ----
APPS := udp_exp udp_client_kernel file_receiver udp_server_kernel

COMMON_SRCS := accnet_lib.c iocache_lib.c 
SRCS := $(COMMON_SRCS) udp_exp.c udp_client_kernel.c file_receiver.c udp_server_kernel.c

COMMON_OBJS := $(COMMON_SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)

# ---- rules ----
.PHONY: all clean strip static

all: $(APPS)

# link each app independently: app := app.o + common objs
$(APPS): %: %.o $(COMMON_OBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -march=$(ARCH) -mabi=$(ABI) $(LDFLAGS) $^ $(LDLIBS) -o $@

# compile .c -> .o with deps
%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -march=$(ARCH) -mabi=$(ABI) -MMD -MP -c $< -o $@

strip: $(APPS)
	$(STRIP) $(APPS)

# Build statically (re-run make with -static)
static:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -static" LDFLAGS="$(LDFLAGS) -static" LDLIBS="$(LDLIBS)"

clean:
	$(RM) $(APPS) $(SRCS:.c=.o) $(DEPS)

-include $(DEPS)
