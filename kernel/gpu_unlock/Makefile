# SPDX-License-Identifier: GPL-2.0-only
#
# Thin wrapper around the kernel build system, so the module can be built
# without a distro packager:
#
#   make                        # the running kernel
#   make TARGET=6.1.0-21-amd64  # another installed kernel
#   make KDIR=/path/to/build    # an explicit kernel build tree
#
# dkms.conf calls this with KDIR set to the tree DKMS selected, so the
# resolution below is only for direct use.

TARGET ?= $(shell uname -r)

# The first kernel tree that actually has a Makefile, in the order the
# common distributions lay their headers out (Debian/Ubuntu, Void, Fedora).
KDIR ?= $(shell ls -d /lib/modules/$(TARGET)/build/Makefile /usr/src/linux-headers-$(TARGET)/Makefile /usr/src/kernel-headers-$(TARGET)/Makefile /usr/src/kernels/$(TARGET)/Makefile 2>/dev/null | head -n 1 | xargs -r dirname)

.PHONY: all modules debug clean

all: modules

modules:
	@test -n "$(KDIR)" || { echo "No kernel build tree for $(TARGET): install the kernel headers, or pass KDIR=/path/to/build." >&2; exit 1; }
	@$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

debug:
	@test -n "$(KDIR)" || { echo "No kernel build tree for $(TARGET): install the kernel headers, or pass KDIR=/path/to/build." >&2; exit 1; }
	@$(MAKE) -C "$(KDIR)" M="$(CURDIR)" ccflags-y+=-DDEBUG modules

clean:
	@$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
