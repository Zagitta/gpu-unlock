#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build gpu_unlock for the booted kernel, package it with its firmware as a
# local RPM and layer it on an rpm-ostree system (Silverblue, Kinoite,
# Bazzite). See "docs/Fedora Atomic and Secure Boot.md" for the manual steps
# this follows.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 [options]

  --targets VALUE   gpu_unlock.targets= kernel argument (default: all)
  --sign            sign the module even if Secure Boot is off
  --no-sign         never sign the module
  --key-dir DIR     signing key location (default: ~/.local/share/gpu-unlock-mok)
  --release VER     Fedora container release for rpmbuild (default: VERSION_ID)
  --build-only      stop after building and checking the RPM
  -y, --yes         do not ask before changing the deployment
  -h, --help        show this help

By default the module is signed only when Secure Boot is on. Changes to the
system all go into one pending deployment; reboot to apply, or pick the
previous entry in the boot menu to go back.
EOF
}

TARGETS=all
SIGN=auto
KEYDIR=$HOME/.local/share/gpu-unlock-mok
BUILD_ONLY=0
YES=0
RELEASE=

while [ $# -gt 0 ]; do
    case "$1" in
        --targets) TARGETS=$2; shift ;;
        --targets=*) TARGETS=${1#*=} ;;
        --sign) SIGN=yes ;;
        --no-sign) SIGN=no ;;
        --key-dir) KEYDIR=$2; shift ;;
        --release) RELEASE=$2; shift ;;
        --build-only) BUILD_ONLY=1 ;;
        -y|--yes) YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

VER=$(cat VERSION)
NV=$(sed -n 's/^NV *:= *//p' Makefile)
KVER=$(uname -r)
KDIR=/usr/src/kernels/$KVER
OUT=$ROOT/build/atomic
CACHE=$HOME/.cache/gpu-unlock
[ -n "$RELEASE" ] || RELEASE=$(. /etc/os-release && echo "$VERSION_ID")

# --- checks -----------------------------------------------------------------

command -v rpm-ostree >/dev/null || die "rpm-ostree not found: this script is for Fedora Atomic systems. Use 'make install' elsewhere."
command -v podman >/dev/null || die "podman is needed to build the RPM in a Fedora container."
[ -d "$KDIR" ] || die "no headers in $KDIR. Layer kernel-devel-$KVER (sudo rpm-ostree install kernel-devel-$KVER) and reboot."
KCORE=$(rpm -q --qf '%{VERSION}-%{RELEASE}' kernel-core) || die "kernel-core is not installed?"
[ "$KCORE.$(uname -m)" = "$KVER" ] || die "kernel-core $KCORE does not match the booted kernel $KVER. Reboot into the latest deployment first."

if ! lspci -n 2>/dev/null | grep -qiE '10de:(22|24|25)[0-9a-f]{2}'; then
    echo "warning: no GA102/GA103/GA104/GA106 card found by lspci; continuing anyway." >&2
fi

SB=off
if command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -q 'SecureBoot enabled'; then
    SB=on
fi
if [ "$SIGN" = auto ]; then
    if [ $SB = on ]; then SIGN=yes; else SIGN=no; fi
fi
[ $SB = on ] && [ $SIGN = no ] && echo "warning: Secure Boot is on and the module will not be signed; the kernel will refuse it." >&2

echo "gpu-unlock $VER, NVIDIA firmware $NV"
echo "kernel     $KVER (kernel-core $KCORE)"
echo "secure boot $SB, signing: $SIGN"
echo "targets    gpu_unlock.targets=$TARGETS"

# --- 1. module and firmware -------------------------------------------------

say "Building the module for $KVER"
make -C kernel/gpu_unlock KDIR="$KDIR" clean >/dev/null
make -C kernel/gpu_unlock KDIR="$KDIR"
KO=kernel/gpu_unlock/gpu_unlock.ko
modinfo -F vermagic "$KO" | grep -q "^$KVER " || die "$KO was not built for $KVER"

say "Generating the firmware (downloads and checks the pinned NVIDIA inputs)"
make assets

# --- 2. signing -------------------------------------------------------------

if [ $SIGN = yes ]; then
    say "Signing the module"
    if [ ! -f "$KEYDIR/mok.key" ]; then
        echo "Creating a signing key in $KEYDIR"
        mkdir -p "$KEYDIR"
        chmod 700 "$KEYDIR"
        (umask 077 && openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
            -subj "/CN=gpu-unlock module signing $(hostname)/" \
            -keyout "$KEYDIR/mok.key" -outform DER -out "$KEYDIR/mok.der")
    fi
    "$KDIR/scripts/sign-file" sha256 "$KEYDIR/mok.key" "$KEYDIR/mok.der" "$KO"
    modinfo "$KO" | grep -E '^(signer|sig_key)'
fi

# --- 3. local RPM -----------------------------------------------------------

say "Building the RPM in a fedora:$RELEASE container"
REL=$(date +%Y%m%d%H%M)
rm -rf "$OUT/src" "$OUT/rpms" "$OUT/gpu-unlock-local.spec"
mkdir -p "$OUT/src/assets" "$OUT/src/licenses" "$OUT/rpms"
cp "$KO" packaging/boot/dracut.conf "$OUT/src/"
for f in booter_load_ga102.bin gsp_ga10x.bin gsp_boot_ga102.bin gsp_boot_desc_ga102.bin "v67-ga102-$NV.bin"; do
    cp "$CACHE/assets/$f" "$OUT/src/assets/"
done
cp licenses/*.txt "$CACHE/assets/NVIDIA-LICENSE" firmware/NOTICE.md "$OUT/src/licenses/"
cp packaging/atomic/gpu-unlock-local.spec "$OUT/"

podman run --rm --security-opt label=disable -v "$OUT:/w" -w /w \
    "registry.fedoraproject.org/fedora:$RELEASE" bash -c "
        dnf install -y -q rpm-build >/dev/null &&
        rpmbuild -bb --define 'kver $KVER' --define 'kcore $KCORE' \
            --define 'ver $VER' --define 'rel $REL' --define 'nv $NV' \
            --define '_sourcedir /w/src' --define '_rpmdir /w/rpms' \
            gpu-unlock-local.spec"

RPM=$(ls "$OUT"/rpms/x86_64/gpu-unlock-local-*.rpm)
echo "Built $RPM"

say "Checking the packaged module"
rpm2cpio "$RPM" | cpio -i --quiet --to-stdout '*gpu_unlock.ko' > "$OUT/check.ko"
cmp "$KO" "$OUT/check.ko" || die "the packaged module differs from the built one"
if [ $SIGN = yes ]; then
    modinfo "$OUT/check.ko" | grep -q '^signer' || die "the packaged module lost its signature"
fi
echo "OK"

if [ $BUILD_ONLY = 1 ]; then
    echo "Stopping here (--build-only)."
    exit 0
fi

# --- 4. layer, initramfs, kargs ----------------------------------------------

ENROLL=no
if [ $SIGN = yes ] && ! mokutil --test-key "$KEYDIR/mok.der" 2>&1 | grep -q 'already enrolled'; then
    ENROLL=yes
fi

INSTALL_ARGS=("$RPM")
if rpm -q gpu-unlock-local >/dev/null 2>&1 || rpm-ostree status --json | grep -q '"gpu-unlock-local'; then
    INSTALL_ARGS=(--uninstall=gpu-unlock-local "$RPM")
fi
INITRAMFS=no
rpm-ostree initramfs | grep -q 'regeneration: enabled' || INITRAMFS=yes
# rpm-ostree refuses a local initramfs while initramfs-etc tracks files;
# hand those to dracut -I instead so they stay in the image.
ETC_FILES=$(rpm-ostree status --json | python3 -c '
import json, sys
print(" ".join(json.load(sys.stdin)["deployments"][0].get("initramfs-etc") or []))')
INITRAMFS_ARGS=(--enable)
[ -n "$ETC_FILES" ] && INITRAMFS_ARGS+=(--arg=-I "--arg=$ETC_FILES")
CURRENT=$(rpm-ostree kargs | tr ' ' '\n' | sed -n 's/^gpu_unlock\.targets=//p' | head -n1)

say "About to change the system"
echo "  - rpm-ostree install ${INSTALL_ARGS[*]}"
if [ $INITRAMFS = yes ]; then
    [ -n "$ETC_FILES" ] && echo "  - rpm-ostree initramfs-etc --untrack-all   (now passed to dracut -I: $ETC_FILES)"
    echo "  - rpm-ostree initramfs ${INITRAMFS_ARGS[*]}   (local initramfs, needed so gpu_unlock loads before nvidia)"
fi
if [ -z "$CURRENT" ]; then
    echo "  - rpm-ostree kargs --append=gpu_unlock.targets=$TARGETS"
elif [ "$CURRENT" != "$TARGETS" ]; then
    echo "  - rpm-ostree kargs --replace=gpu_unlock.targets=$TARGETS   (currently $CURRENT)"
fi
[ $ENROLL = yes ] && echo "  - mokutil --import $KEYDIR/mok.der   (asks for a one-time password)"

if [ $YES = 0 ]; then
    read -r -p "Continue? [y/N] " answer
    case "$answer" in y|Y|yes|o|O|oui) ;; *) echo "Aborted."; exit 1 ;; esac
fi

sudo rpm-ostree install "${INSTALL_ARGS[@]}"
if [ $INITRAMFS = yes ]; then
    [ -n "$ETC_FILES" ] && sudo rpm-ostree initramfs-etc --untrack-all
    sudo rpm-ostree initramfs "${INITRAMFS_ARGS[@]}"
fi
if [ -z "$CURRENT" ]; then
    sudo rpm-ostree kargs --append="gpu_unlock.targets=$TARGETS"
elif [ "$CURRENT" != "$TARGETS" ]; then
    sudo rpm-ostree kargs --replace="gpu_unlock.targets=$TARGETS"
fi
if [ $ENROLL = yes ]; then
    sudo mokutil --import "$KEYDIR/mok.der"
    echo "On the next boot, the blue MOK Manager screen asks for that password: Enroll MOK -> Continue -> Yes."
fi

say "Checking the pending initramfs"
COMMIT=$(rpm-ostree status --json | python3 -c '
import json, sys
print(json.load(sys.stdin)["deployments"][0]["checksum"])')
sudo ostree --repo=/ostree/repo cat "$COMMIT" "/usr/lib/modules/$KVER/initramfs.img" > "$OUT/initramfs.img"
lsinitrd "$OUT/initramfs.img" 2>/dev/null > "$OUT/initramfs.txt" || :
rm -f "$OUT/initramfs.img"
grep -q 'extra/gpu_unlock.ko' "$OUT/initramfs.txt" || die "gpu_unlock.ko is not in the pending initramfs; it would load after nvidia."
grep -q "firmware/gpu-unlock/$NV/v67-ga102-$NV.bin" "$OUT/initramfs.txt" || die "the gpu-unlock firmware is not in the pending initramfs."
PRE=$(sed -n 's/^ *rd\.driver\.pre=//p' "$OUT/initramfs.txt")
[ "$(echo "$PRE" | grep -m1 -xE 'gpu_unlock|nvidia')" = gpu_unlock ] ||
    die "the initramfs loads nvidia before gpu_unlock; the unlock would fail with -EBUSY. Order: $(echo $PRE)"
echo "OK: module and firmware are in the initramfs, gpu_unlock loads before nvidia"

cat <<EOF

Done. Reboot to apply (systemctl reboot), then check:

    journalctl -k -b | grep -E 'gpu_unlock|selector'

A working boot prints 'selector write: SS0=0x88888888', 'SS1=0x8' and
'gpu_unlock: unlocked'.

When an image update brings a new kernel, the update stops at the
kernel-core dependency. Run 'rpm-ostree upgrade --uninstall=gpu-unlock-local',
reboot, and run this script again.
EOF
