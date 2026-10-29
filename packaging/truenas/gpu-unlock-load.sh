#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Load gpu_unlock for one card before nvidia binds it. Runs from the unpacked
# bundle as an ExecStartPre of middlewared.service, which loads nvidia. The
# argument is the card's PCI address as lspci -D prints it; one card only.
# It never fails the boot: every problem is logged and it exits 0. It does
# nothing when gpu_unlock.disable is on the kernel command line, no card is
# given, the kernel differs from the one the module was built for, the card
# is missing or already bound, or the bundle checksums do not match.
DIR=$(dirname "$(readlink -f "$0")")
GPU=${1:-}
FWPATH=/sys/module/firmware_class/parameters/path

log() { logger -s -t gpu-unlock "$*"; }

# One-off escape hatch: add gpu_unlock.disable in the GRUB edit screen.
if grep -qw gpu_unlock.disable /proc/cmdline; then
	log "gpu_unlock.disable on kernel command line: skipped"
	exit 0
fi
if [ -z "$GPU" ]; then
	log "no card PCI address given: skipped"
	exit 0
fi
if [ "$(uname -r)" != "$(cat "$DIR/KERNEL")" ]; then
	log "built for $(cat "$DIR/KERNEL"), running $(uname -r): skipped"
	exit 0
fi
if [ ! -e "/sys/bus/pci/devices/$GPU" ]; then
	log "no PCI device $GPU: skipped"
	exit 0
fi
if [ -e "/sys/bus/pci/devices/$GPU/driver" ]; then
	log "$GPU already bound to $(basename "$(readlink "/sys/bus/pci/devices/$GPU/driver")"): skipped"
	exit 0
fi
if grep -q '^gpu_unlock ' /proc/modules; then
	log "gpu_unlock already loaded: skipped"
	exit 0
fi
if ! (cd "$DIR" && sha256sum --quiet -c SHA256SUMS); then
	log "bundle checksum mismatch: skipped"
	exit 0
fi

# Point the firmware loader at the bundle only for the duration of the load.
old=$(cat "$FWPATH")
printf '%s' "$DIR/firmware" > "$FWPATH"
insmod "$DIR/gpu_unlock.ko" targets="$GPU"
rc=$?
# echo, not printf: the kernel ignores an empty write, and the loader strips the newline.
echo "$old" > "$FWPATH"

log "insmod exit $rc; kernel log:"
dmesg | grep -e gpu_unlock -e "$GPU" | tail -n 5 | while read -r line; do log "$line"; done
exit 0
