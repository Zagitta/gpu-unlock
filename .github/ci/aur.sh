#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run as root in a disposable Arch container. Never load the module.
set -eu

work=$(mktemp -d /tmp/gpu-unlock-aur.XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM

# A separate directory ensures the full checkout cannot mask missing AUR files.
install -m644 packaging/aur/PKGBUILD packaging/aur/.SRCINFO \
    packaging/aur/gpu-unlock.install "$work/"
useradd -m builder
chown -R builder "$work"
runuser -u builder -- bash -euc '
    cd "$1"
    makepkg --printsrcinfo > .SRCINFO.generated
    diff -u .SRCINFO .SRCINFO.generated
    PKGEXT=.pkg.tar.zst makepkg --cleanbuild --clean --noconfirm
' sh "$work"

set -- "$work"/*.pkg.tar.zst
test "$#" -eq 2
pacman -U --noconfirm "$@"

version=$(awk '$1 == "pkgver" { print $3; exit }' "$work/.SRCINFO")
test -n "$version"
test -f "/usr/src/gpu-unlock-$version/dkms.conf"
test -f "/usr/src/gpu-unlock-$version/main.c"
test -f /etc/mkinitcpio.conf.d/zz-gpu-unlock.conf
test -f /usr/lib/dracut/dracut.conf.d/gpu-unlock.conf
test -f /usr/lib/firmware/gpu-unlock/*/gsp_ga10x.bin
test -f /usr/lib/firmware/gpu-unlock/*/booter_load_ga102.bin
test -s /usr/share/licenses/gpu-unlock-firmware/NVIDIA-LICENSE

# Headers suffice for this compile/install check; do not boot a kernel or GPU.
tested_kernel=
for build in /usr/lib/modules/*/build; do
    test -f "$build/Makefile" || continue
    kernel=${build%/build}
    kernel=${kernel##*/}
    dkms install -m gpu-unlock -v "$version" -k "$kernel"
    # Headers-only containers may not have a module index yet.
    depmod -a "$kernel"
    test "$(modinfo -k "$kernel" -F name gpu_unlock)" = gpu_unlock
    tested_kernel=$kernel
done
test -n "$tested_kernel"

pacman -Rns --noconfirm gpu-unlock-dkms gpu-unlock-firmware
test ! -e "/usr/src/gpu-unlock-$version"
test ! -e /usr/lib/firmware/gpu-unlock
test ! -e /etc/mkinitcpio.conf.d/zz-gpu-unlock.conf
test ! -e /usr/lib/dracut/dracut.conf.d/gpu-unlock.conf
for build in /usr/lib/modules/*/build; do
    test -f "$build/Makefile" || continue
    kernel=${build%/build}
    kernel=${kernel##*/}
    if modinfo -k "$kernel" gpu_unlock >/dev/null 2>&1; then
        echo "gpu_unlock remains installed for $kernel" >&2
        exit 1
    fi
done
echo "Standalone AUR packages build, install, compile with DKMS and remove cleanly."
