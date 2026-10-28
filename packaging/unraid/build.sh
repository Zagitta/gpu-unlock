#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the Unraid package of gpu_unlock for one Unraid kernel. The source
# is /usr/src/linux-*-Unraid from the bzmodules of an Unraid 7.3 or later
# release: the kernel config, Unraid's patches and drivers overlay, and
# System.map. Needs curl, patch, xz, kmod and what a kernel build needs,
# with the gcc major version the kernel was built with. Run from the
# repository root:
#   packaging/unraid/build.sh <linux-*-Unraid> <output directory>
set -eu
src=$(realpath "$1")
out=$(mkdir -p "$2" && realpath "$2")
krel=${src##*/linux-}
kver=${krel%%-*}
version=$(cat VERSION)
nv=$(sed -n 's/^NV *:= *//p' Makefile)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The vanilla kernel with Unraid's changes, prepared for external modules.
curl -sSfL -o "$work/linux.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v${kver%%.*}.x/linux-$kver.tar.xz"
tar -xJf "$work/linux.tar.xz" -C "$work"
kdir=$work/linux-$kver
cp -r "$src/drivers/." "$kdir/drivers/"
for patch in "$src"/*.patch; do
	patch -s -p1 -d "$kdir" <"$patch"
done
cp "$src/config" "$kdir/.config"
make -s -C "$kdir" olddefconfig
make -s -C "$kdir" -j"$(nproc)" modules_prepare
[ "$(make -s -C "$kdir" kernelrelease)" = "$krel" ] || {
	echo "kernel release is $(make -s -C "$kdir" kernelrelease), not $krel" >&2
	exit 1
}

# Without a full kernel build there is no Module.symvers, so modpost cannot
# resolve symbols; Unraid has CONFIG_MODVERSIONS off, and every symbol the
# module uses is checked against the kernel's exports instead.
cp -r kernel/gpu_unlock "$work/module"
make -C "$work/module" KDIR="$kdir" KBUILD_MODPOST_WARN=1
ko=$work/module/gpu_unlock.ko
modinfo -F vermagic "$ko" | grep -q "^$krel " || {
	echo "vermagic is $(modinfo -F vermagic "$ko"), not $krel" >&2
	exit 1
}
for sym in $(nm -u "$ko" | awk '{print $2}'); do
	grep -q " __ksymtab_$sym\$" "$src/System.map" || {
		echo "$krel does not export $sym" >&2
		exit 1
	}
done

# The module, and what gpu-unlock-firmware needs to generate the firmware.
pkg=$work/pkg
share=$pkg/usr/local/share/gpu-unlock
install -Dm644 "$ko" "$pkg/lib/modules/$krel/extra/gpu_unlock.ko"
install -Dm644 -t "$share" firmware/fetch_artifacts.py firmware/extract_gsp.py \
	firmware/extract_boot_assets.py firmware/NOTICE.md licenses/*.txt
mkdir -p "$pkg/usr/local/sbin"
sed "s/@NV@/$nv/" packaging/unraid/gpu-unlock-firmware >"$pkg/usr/local/sbin/gpu-unlock-firmware"
chmod 755 "$pkg/usr/local/sbin/gpu-unlock-firmware"
name=gpu-unlock-${version}_$(echo "$krel" | tr - _)-x86_64-1.txz
# Slackware packages list their files without a leading ./
tar --owner=0 --group=0 --sort=name --xform='sx^\./\(.\)x\1x' \
	-C "$pkg" -cJf "$out/$name" .
(cd "$out" && md5sum "$name" >"$name.md5")
echo "$out/$name"
