#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Collect what build.sh needs from a TrueNAS host, plus the checks the README
# asks for. Run on the TrueNAS host as root:  sudo bash host-diagnostics.sh [outdir]
# Writes report.txt and kernel-headers.tar.gz to outdir (default /tmp/gpu-unlock-diag).
# Changes nothing on the host apart from creating outdir.
set -euo pipefail
OUT=${1:-/tmp/gpu-unlock-diag}
KVER=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "run as root: sudo bash $0" >&2; exit 1; }
mkdir -p "$OUT"

{
	echo "TrueNAS: $(cat /etc/version)  kernel: $KVER"
	echo "sig_enforce: $(cat /sys/module/module/parameters/sig_enforce)  (must be N)"
	printf 'docker nvidia: '
	midclt call docker.config | python3 -c "import json,sys; print(json.load(sys.stdin)['nvidia'])"
	for gpu in $(lspci -D -n -d 10de::0300 | cut -d' ' -f1); do
		flr=no
		lspci -vvv -s "$gpu" | grep -q 'FLReset+' && flr=yes
		echo "card $gpu: $(lspci -nn -s "$gpu" | cut -d' ' -f2-)  FLReset+: $flr"
	done
} | tee "$OUT/report.txt"

# Dereference symlinks so the tarball holds the real files.
tar -C "$(realpath "/lib/modules/$KVER/build")" -chzf "$OUT/kernel-headers.tar.gz" .
echo "Done. Copy $OUT to the build machine and run:"
echo "  packaging/truenas/build.sh <copied dir>"
