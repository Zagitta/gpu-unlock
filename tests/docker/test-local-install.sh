#!/bin/sh
# Install the tree with the packager-free path in a fresh container and undo it.
# SRC is the checkout to install and INPUTS the NVIDIA input cache; both are
# mounted paths in tests/docker.sh and the workspace in CI.
set -eu
commit=$1
src=${SRC:-/src}
inputs=${INPUTS:-/inputs}
apt-get update -q
apt-get install -qy --no-install-recommends \
	ca-certificates git make gcc dkms linux-headers-amd64 initramfs-tools \
	python3 zstd xz-utils
git config --global --add safe.directory '*'
git clone -q --shared "$src" /work
cd /work
git checkout -q "$commit"
cp -r "$inputs" /tmp/inputs

k=$(ls -d /lib/modules/*/build 2>/dev/null | head -n1); k=${k%/build}; k=${k##*/}
echo "kernel: $k (running $(uname -r))"

# The deb's own hooks install the initramfs drop-ins; the packager-free target
# installs the module, its firmware and the licenses, so seed the drop-ins that
# the deb owns and assert the half install-local owns.
seed_boot() {
	git -C /work ls-files 'packaging/boot/*' | while read -r f; do
		install -Dm644 "/work/$f" "/tmp/boot/$(basename "$f")"
	done
	mkdir -p /usr/share/initramfs-tools/modules.d /usr/share/initramfs-tools/scripts/init-top
	cp /tmp/boot/initramfs-tools.conf /usr/share/initramfs-tools/modules.d/gpu-unlock.conf
	cp /tmp/boot/initramfs-tools-init-top /usr/share/initramfs-tools/scripts/init-top/gpu-unlock
}

make install-local INPUTS=/tmp/inputs SUDO= TARGET="$k" BOOT=initramfs-tools
seed_boot
v=$(cat VERSION)
test -f "/usr/src/gpu-unlock-$v/dkms.conf"
test -f /usr/lib/firmware/gpu-unlock/610.43.03/gsp_ga10x.bin
test -f /usr/lib/firmware/gpu-unlock/610.43.03/booter_load_tu102.bin
test -f /usr/share/licenses/gpu-unlock-firmware/NVIDIA-LICENSE
# Debian's depmod.d compresses installed modules, so DKMS leaves gpu_unlock.ko.xz.
if dkms status | grep -q "gpu-unlock/$v, $k,.*: installed"; then
	echo dkms:installed
else
	echo "dkms did not install the module for $k" >&2
	dkms status >&2
	exit 1
fi
modinfo -k "$k" -F name gpu_unlock
modinfo -k "$k" -F version gpu_unlock
update-initramfs -u -k "$k"
d=$(mktemp -d)
unmkinitramfs "/boot/initrd.img-$k" "$d" >/dev/null 2>&1
find "$d" -name 'gpu_unlock.ko*' | grep -q . || { echo "image: no module" >&2; exit 1; }
echo image:has-module
find "$d" -path '*gpu-unlock*booter_load_tu102.bin' | grep -q . || {
	echo "image: no firmware" >&2
	exit 1
}
echo image:has-firmware
rm -rf "$d"

make uninstall-local SUDO= TARGET="$k"
test ! -e "/usr/src/gpu-unlock-$(cat VERSION)"
test ! -e /usr/lib/firmware/gpu-unlock
test ! -e /usr/share/licenses/gpu-unlock-firmware
test ! -e /usr/share/licenses/gpu-unlock
test ! -e /usr/share/doc/gpu-unlock/README.md
test ! -e /usr/share/initramfs-tools/modules.d/gpu-unlock.conf
test ! -e /usr/share/initramfs-tools/scripts/init-top/gpu-unlock
test ! -e /usr/share/initramfs-tools/hooks/gpu-unlock
test ! -e /usr/share/initramfs-tools/scripts/init-premount/gpu-unlock
test ! -e /usr/lib/dracut/dracut.conf.d/00-gpu-unlock.conf
if dkms status | grep -q gpu-unlock; then
	echo "dkms left registered" >&2
	dkms status >&2
	exit 1
fi
echo LOCAL-INSTALL-ROUNDTRIP-OK
