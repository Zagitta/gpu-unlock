# SPDX-License-Identifier: GPL-2.0-only
VERSION    := $(shell cat VERSION)
NV         := 610.43.03
PYTHON     ?= python3
DESTDIR    ?=
DISTDIR    ?= build
CACHE      ?= $(HOME)/.cache/gpu-unlock
INPUTS     ?= $(CACHE)/inputs
ASSETS     ?= $(CACHE)/assets
DKMSDIR    ?= /usr/src/gpu-unlock-$(VERSION)
FWDIR      ?= /usr/lib/firmware/gpu-unlock/$(NV)
LICENSEDIR ?= /usr/share/licenses
PKG        ?= gpu-unlock

MODULE_FILES   := $(filter-out %.mod.c,$(sort $(wildcard kernel/gpu_unlock/*.[ch]))) \
                  kernel/gpu_unlock/Kbuild kernel/gpu_unlock/Makefile kernel/gpu_unlock/dkms.conf
FIRMWARE_FILES := booter_load_ad102.bin booter_load_ga102.bin gsp_boot_ad102.bin gsp_boot_desc_ad102.bin \
                  booter_load_tu102.bin gsp_boot_tu102.bin gsp_boot_desc_tu102.bin sec2_bl_tu102.bin \
                  booter_load_ga100.bin falcon_loader.bin gsp_ga10x.bin gsp_tu10x.bin \
                  gsp_boot_ga102.bin gsp_boot_desc_ga102.bin \
                  gsp_boot_ga100.bin gsp_boot_desc_ga100.bin
LICENSES       := licenses/GPL-2.0.txt licenses/CMP40HX-MIT.txt licenses/NVIDIA-MIT.txt
NV_SOURCE      := NVIDIA-kernel-module-source-$(NV)
NV_INSTALLER   := NVIDIA-Linux-x86_64-$(NV)-no-compat32.run

TARBALL := $(abspath $(DISTDIR))/gpu-unlock-$(VERSION).tar.xz
DEBDIR  := $(abspath $(DISTDIR))/deb
RPMDIR  := $(abspath $(DISTDIR))/rpm
DEBS    := $(foreach p,dkms firmware,$(DEBDIR)/gpu-unlock-$(p)_$(VERSION)_all.deb)

.PHONY: all dist inputs firmware assets install-dkms install-firmware \
        install-mkinitcpio install-initramfs-tools install-dracut \
        install-licenses arch deb rpm install install-arch install-deb install-rpm \
        test-module test-packages \
        modules clean-modules dkms-install dkms-uninstall install-boot \
        rebuild-initramfs install-local uninstall-local insmod rmmod status \
        install-none rebuild-dracut rebuild-initramfs-tools rebuild-mkinitcpio rebuild-none

all:

# Source-only release tarball; see .gitattributes for what is left out.
dist:
	mkdir -p $(DISTDIR)
	git archive --format=tar --prefix=gpu-unlock-$(VERSION)/ \
		--output=$(DISTDIR)/gpu-unlock-$(VERSION).tar "$$(packaging/snapshot.sh)"
	xz -9 -f $(DISTDIR)/gpu-unlock-$(VERSION).tar

# Download the NVIDIA inputs. makepkg does this itself from the PKGBUILD's
# source array.
inputs:
	$(PYTHON) firmware/fetch_artifacts.py $(INPUTS)

# Generate firmware from $(INPUTS) into $(ASSETS); debian/rules and the RPM
# spec call this from their build step.
firmware:
	rm -rf $(ASSETS).src
	mkdir -p $(ASSETS).src
	tar -xf $(INPUTS)/$(NV_SOURCE).tar.xz -C $(ASSETS).src
	$(PYTHON) firmware/extract_gsp.py "$$(realpath $(INPUTS)/$(NV_INSTALLER))" $(ASSETS)
	$(PYTHON) firmware/extract_boot_assets.py $(ASSETS).src/$(NV_SOURCE) $(ASSETS)

assets: inputs
	$(MAKE) firmware

# The module source. It does nothing unless gpu_unlock.targets= is on the
# kernel command line.
install-dkms:
	grep -qx 'PACKAGE_VERSION="$(VERSION)"' kernel/gpu_unlock/dkms.conf
	grep -q 'MODULE_VERSION("$(VERSION)")' kernel/gpu_unlock/main.c
	install -Dm644 -t $(DESTDIR)$(DKMSDIR) $(MODULE_FILES)
	install -Dm644 README.md $(DESTDIR)/usr/share/doc/gpu-unlock/README.md

install-firmware:
	install -Dm644 -t $(DESTDIR)$(FWDIR) $(addprefix $(ASSETS)/,$(FIRMWARE_FILES))
	install -Dm644 $(ASSETS)/NVIDIA-LICENSE $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NVIDIA-LICENSE
	install -Dm644 firmware/NOTICE.md $(DESTDIR)$(LICENSEDIR)/gpu-unlock-firmware/NOTICE.md

# Per initramfs generator: include gpu_unlock and its firmware in
# the image so it runs before an nvidia module loaded from the initramfs.
install-mkinitcpio:
	install -Dm644 packaging/boot/mkinitcpio.conf $(DESTDIR)/etc/mkinitcpio.conf.d/zz-gpu-unlock.conf

install-initramfs-tools:
	install -Dm644 packaging/boot/initramfs-tools.conf \
		$(DESTDIR)/usr/share/initramfs-tools/modules.d/gpu-unlock.conf
	install -Dm755 packaging/boot/initramfs-tools-init-top \
		$(DESTDIR)/usr/share/initramfs-tools/scripts/init-top/gpu-unlock

install-dracut:
	install -Dm644 packaging/boot/dracut.conf $(DESTDIR)/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf

install-licenses:
	install -Dm644 -t $(DESTDIR)$(LICENSEDIR)/$(PKG) $(LICENSES)

# --- Distro-agnostic build and install --------------------------------
#
# Stage the module source in /usr/src, let DKMS build and install it, drop
# the generated firmware, the licenses and the initramfs hook in place,
# then rebuild the initramfs. Needs dkms, kernel headers, python3 and
# zstd, and no distro packager at all.
#
#   TARGET=<uname -r>   one kernel (default: the running one)
#   TARGET=all          every installed kernel DKMS can build for
#   BOOT=auto|dracut|mkinitcpio|initramfs-tools
#   SUDO=sudo           set SUDO= to run the privileged steps as root already

TARGET ?= $(shell uname -r)
SUDO   ?= sudo
DKMS   ?= dkms
BOOT   ?= auto

# One kernel with -k, every installed one with --all.
DKMS_ARGS := $(shell [ "$(TARGET)" = all ] && echo --all || echo -k $(TARGET))
# The kernels to rebuild initramfs images for.
TARGET_KERNELS := $(shell [ "$(TARGET)" = all ] && ls /lib/modules || echo $(TARGET))

# Build the .ko in the tree: no DKMS, no install.
modules:
	@$(MAKE) -C kernel/gpu_unlock TARGET=$(TARGET)

clean-modules:
	@$(MAKE) -C kernel/gpu_unlock TARGET=$(TARGET) clean

dkms-install:
	@$(SUDO) $(MAKE) install-dkms DESTDIR=/
	@set -eu; \
	$(SUDO) $(DKMS) add $(DKMSDIR); \
	$(SUDO) $(DKMS) build $(PKG)/$(VERSION) $(DKMS_ARGS); \
	$(SUDO) $(DKMS) install $(PKG)/$(VERSION) $(DKMS_ARGS)

dkms-uninstall:
	@set -eu; \
	$(SUDO) $(DKMS) uninstall $(PKG)/$(VERSION) $(DKMS_ARGS) || true; \
	$(SUDO) $(DKMS) remove $(PKG)/$(VERSION) $(DKMS_ARGS) || true; \
	$(SUDO) rm -rf $(DKMSDIR)

# The initramfs generator to use: the one asked for, or the one installed.
BOOT_RESOLVED := $(shell [ "$(BOOT)" != auto ] && echo $(BOOT) || { command -v dracut >/dev/null 2>&1 && echo dracut || { command -v update-initramfs >/dev/null 2>&1 && echo initramfs-tools || { command -v mkinitcpio >/dev/null 2>&1 && echo mkinitcpio || echo none; }; }; })

# The hook for whichever initramfs generator this system runs. install-boot
# and rebuild-initramfs pick the per-generator target by name, so BOOT=
# also selects one directly.
install-boot:
	@$(SUDO) $(MAKE) install-$(BOOT_RESOLVED) DESTDIR=/

install-none:
	@echo "No known initramfs generator here. Install dracut, initramfs-tools or mkinitcpio, or pass BOOT=<name>." >&2
	@exit 1

rebuild-initramfs:
	@$(MAKE) rebuild-$(BOOT_RESOLVED) SUDO="$(SUDO)" TARGET_KERNELS="$(TARGET_KERNELS)"

rebuild-dracut:
	@$(foreach k,$(TARGET_KERNELS),$(SUDO) dracut --force --kver $(k); )

rebuild-initramfs-tools:
	@$(foreach k,$(TARGET_KERNELS),$(SUDO) update-initramfs -u -k $(k); )

rebuild-mkinitcpio:
	@$(SUDO) mkinitcpio -P

rebuild-none:
	@echo "No initramfs tool found; rebuild the images by hand." >&2

install-local: assets
	@$(MAKE) dkms-install SUDO="$(SUDO)"
	@$(SUDO) $(MAKE) install-firmware install-licenses DESTDIR=/
	@$(MAKE) install-boot BOOT=$(BOOT) SUDO="$(SUDO)"
	@$(MAKE) rebuild-initramfs TARGET=$(TARGET) SUDO="$(SUDO)"
	@echo "Installed. Opt in on the kernel command line: gpu_unlock.targets=all"

uninstall-local:
	@$(MAKE) dkms-uninstall SUDO="$(SUDO)"
	$(SUDO) rm -rf $(FWDIR) $(LICENSEDIR)/$(PKG) $(LICENSEDIR)/gpu-unlock-firmware
	$(SUDO) rm -f /etc/mkinitcpio.conf.d/zz-gpu-unlock.conf \
		/usr/share/initramfs-tools/modules.d/gpu-unlock.conf \
		/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf
	@$(MAKE) rebuild-initramfs TARGET=$(TARGET) SUDO="$(SUDO)"

# Load the locally built module by hand. It does nothing to a card the
# nvidia driver already bound; pass targets= to select cards.
insmod: modules
	-$(SUDO) rmmod gpu_unlock
	$(SUDO) insmod kernel/gpu_unlock/gpu_unlock.ko $(ARGS)

rmmod:
	-$(SUDO) rmmod gpu_unlock

status:
	@echo "--- dkms"; $(DKMS) status 2>/dev/null | grep $(PKG) || echo "  not registered"
	@echo "--- loaded"; lsmod | grep gpu_unlock || echo "  not loaded"
	@echo "--- built"; modinfo kernel/gpu_unlock/gpu_unlock.ko 2>/dev/null | head -n 4 || echo "  make modules"
	@echo "--- firmware"; ls $(ASSETS) 2>/dev/null || echo "  make assets"

# Native packages, built from the working tree like the PKGBUILD. The
# install-* variants also pull build dependencies and install the result.
install:
	@set -eu; \
	if [ -r /etc/os-release ]; then \
		. /etc/os-release; \
	elif [ -r /usr/lib/os-release ]; then \
		. /usr/lib/os-release; \
	fi; \
	for _distro in $${ID:-} $${ID_LIKE:-}; do \
		case "$$_distro" in \
			arch) echo "Detected $$_distro; running make install-arch"; $(MAKE) install-arch; exit 0 ;; \
			debian|ubuntu) echo "Detected $$_distro; running make install-deb"; $(MAKE) install-deb; exit 0 ;; \
			fedora|rhel|centos) echo "Detected $$_distro; running make install-rpm"; $(MAKE) install-rpm; exit 0 ;; \
			*) continue ;; \
		esac; \
	done; \
	if command -v dkms >/dev/null 2>&1; then \
		echo "No native packager for $${ID:-unknown}; using the DKMS install."; \
		$(MAKE) install-local; exit 0; \
	fi; \
	echo "Unsupported distribution: $${ID:-unknown}. Use make install-arch, install-deb, install-rpm, or install-local (needs dkms)." >&2; \
	exit 1

arch:
	cd packaging/arch && makepkg -f

install-arch:
	cd packaging/arch && makepkg -sif

deb: dist inputs
	rm -rf $(DEBDIR)
	mkdir -p $(DEBDIR)
	tar -xf $(TARBALL) -C $(DEBDIR)
	cd $(DEBDIR)/gpu-unlock-$(VERSION) && \
		GPU_UNLOCK_INPUTS=$(abspath $(INPUTS)) dpkg-buildpackage -us -uc

install-deb:
	sudo apt-get build-dep -y $(CURDIR)
	$(MAKE) deb
	sudo apt-get install -y $(DEBS)

rpm: dist inputs
	rm -rf $(RPMDIR)
	mkdir -p $(RPMDIR)/SOURCES
	cp $(TARBALL) $(RPMDIR)/SOURCES/
	ln -s $(abspath $(INPUTS))/$(NV_SOURCE).tar.xz $(abspath $(INPUTS))/$(NV_INSTALLER) $(RPMDIR)/SOURCES/
	rpmbuild -ba --define "_topdir $(RPMDIR)" packaging/rpm/gpu-unlock.spec

install-rpm:
	sudo dnf install -y rpm-build
	sudo dnf builddep -y packaging/rpm/gpu-unlock.spec
	$(MAKE) rpm
	sudo dnf install -y $(RPMDIR)/RPMS/noarch/gpu-unlock-*-$(VERSION)-*.noarch.rpm

# Build the module, or build, install and remove the packages, in a
# container of each distribution.
test-module:
	tests/docker.sh module

test-packages:
	INPUTS=$(INPUTS) tests/docker.sh package
