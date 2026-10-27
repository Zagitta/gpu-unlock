#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the module against every set of distribution kernel headers this
# container offers, with warnings as errors. Run from the repository root.
set -eu
. /etc/os-release
case "$ID" in
debian)
	apt-get update -q
	apt-get install -qy --no-install-recommends make gcc linux-headers-amd64 ;;
ubuntu)
	apt-get update -q
	apt-get install -qy --no-install-recommends make gcc linux-headers-generic
	# The hardware enablement kernel, where the release has one.
	apt-get install -qy --no-install-recommends "linux-headers-generic-hwe-$VERSION_ID" || :
	# Its headers do not pull in the gcc it was built with.
	apt-get install -qy --no-install-recommends $(grep -h '^CONFIG_CC_VERSION_TEXT=' \
		/usr/src/linux-headers-*/.config | grep -o 'gcc-[0-9]*' | sort -u) ;;
fedora)
	dnf install -qy make gcc kernel-devel ;;
opensuse*)
	zypper -n install make gcc kernel-default-devel ;;
arch)
	pacman -Syu --noconfirm --needed make gcc linux-headers linux-lts-headers ;;
*)
	echo "unknown distribution: $ID" >&2
	exit 1 ;;
esac
# The same headers can show up under more than one of these paths.
built=
for kdir in /usr/src/linux-headers-*-amd64 /usr/src/linux-headers-*-generic \
	    /usr/src/kernels/* /usr/lib/modules/*/build; do
	[ -f "$kdir/Makefile" ] || continue
	kdir=$(realpath "$kdir")
	case " $built " in *" $kdir "*) continue ;; esac
	echo "Building against $kdir"
	make -C kernel/gpu_unlock KDIR="$kdir" clean
	make -C kernel/gpu_unlock KDIR="$kdir" KCFLAGS=-Werror
	test -f kernel/gpu_unlock/gpu_unlock.ko
	built="$built $kdir"
done
[ -n "$built" ] || { echo "no kernel headers found" >&2; exit 1; }
