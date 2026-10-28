#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build gpu_unlock.ko for a TrueNAS SCALE kernel and bundle it with its
# firmware and loader. Runs on any Linux machine with Docker and Python 3, not
# on TrueNAS: the compile happens in a debian:bookworm container because
# TrueNAS kernels are built with Debian 12's GCC 12.2.
#
# usage: packaging/truenas/build.sh <hostdir>
#   hostdir holds kernel-headers.tar.gz from host-diagnostics.sh.
#   The bundle goes to packaging/truenas/out/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/packaging/truenas
HOST=$(realpath "${1:?usage: $0 <dir with kernel-headers.tar.gz>}")
WORK=$HERE/work
OUT=$HERE/out
IMAGE=${IMAGE:-debian:bookworm}
NV=$(sed -n 's/^NV *:= *//p' "$ROOT/Makefile")

[ -f "$HOST/kernel-headers.tar.gz" ] || { echo "missing $HOST/kernel-headers.tar.gz" >&2; exit 1; }

# 1. Firmware, generated from the pinned NVIDIA inputs (cached in work/ per NV).
ASSETS=$WORK/assets-$NV
mkdir -p "$WORK"
if [ ! -f "$ASSETS/gsp_ga10x.bin" ]; then
	make -C "$ROOT" inputs INPUTS="$WORK/inputs-$NV"
	make -C "$ROOT" firmware INPUTS="$WORK/inputs-$NV" ASSETS="$ASSETS"
fi
rm -rf "$ASSETS.src"

# 2. Fresh copies of the headers and module source; upstream files stay untouched.
rm -rf "$WORK/headers" "$WORK/module"
mkdir -p "$WORK/headers" "$WORK/module"
tar -xzf "$HOST/kernel-headers.tar.gz" -C "$WORK/headers"
cp "$ROOT"/kernel/gpu_unlock/{*.c,*.h,Kbuild,Makefile} "$WORK/module/"

# 3. Compile inside Debian 12. Root only installs packages; the
# build runs as the calling user so nothing in the checkout ends up root-owned.
BUILD='
set -euo pipefail
K=/work/headers
cd /work/module
# Compare from the "(Debian ...) x.y.z" part on; the binary name may differ.
want=$(sed -n "s/^CONFIG_CC_VERSION_TEXT=\"\{0,1\}\([^\"]*\)\"\{0,1\}$/\1/p" $K/include/config/auto.conf)
have=$(gcc --version | head -1)
echo "kernel compiler: $want"
echo "build compiler:  $have"
[ -n "$want" ] && [ "(${want#*(}" = "(${have#*(}" ] || { echo "FAIL: compiler mismatch" >&2; exit 1; }

# modpost rejects undefined symbols, as the headers include Module.symvers.
make -C $K M=$PWD modules
modinfo ./gpu_unlock.ko
sed -n "s/^#define UTS_RELEASE \"\(.*\)\"/\1/p" $K/include/generated/utsrelease.h > /work/module/KERNEL
'
docker run --rm -v "$WORK:/work" "$IMAGE" bash -euc '
apt-get update -q >/dev/null
apt-get install -qy --no-install-recommends gcc-12 make kmod libelf1 binutils util-linux >/dev/null
ln -sf gcc-12 /usr/bin/gcc
exec setpriv --reuid="$1" --regid="$2" --clear-groups bash -c "$3"
' sh "$(id -u)" "$(id -g)" "$BUILD" 2>&1 | tee "$WORK/build.log"

# 4. Bundle: module, firmware at the path request_firmware() asks for, loader.
KVER=$(cat "$WORK/module/KERNEL")
B=$OUT/gpu-unlock-$KVER
rm -rf "$B"
mkdir -p "$B/firmware/gpu-unlock/$NV"
cp "$WORK/module/gpu_unlock.ko" "$WORK/module/KERNEL" "$HERE/gpu-unlock-load.sh" "$B/"
cp "$ASSETS"/*.bin "$B/firmware/gpu-unlock/$NV/"
cp "$ASSETS/NVIDIA-LICENSE" "$ROOT/firmware/NOTICE.md" "$ROOT"/licenses/*.txt "$B/"
{
	echo "gpu-unlock $(cat "$ROOT/VERSION") for $KVER"
	echo "built $(date -u +%FT%TZ) in $IMAGE"
	echo "source commit $(git -C "$ROOT" -c safe.directory="$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	grep -E '^(kernel|build) compiler' "$WORK/build.log"
} > "$B/BUILDINFO"
(cd "$B" && find . -type f ! -name SHA256SUMS -printf '%P\n' | sort | xargs sha256sum > SHA256SUMS)
tar -C "$OUT" -czf "$B.tar.gz" "$(basename "$B")"
echo "Bundle: $B.tar.gz"
