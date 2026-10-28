#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Read-only diagnostics for the gpu-unlock TrueNAS integration.
# Run on the TrueNAS host as root:  sudo bash host-diagnostics.sh [outdir]
# Writes report.txt and kernel-headers.tar.gz to outdir (default /tmp/gpu-unlock-diag).
# Changes nothing on the host apart from creating outdir.
set -u
OUT=${1:-/tmp/gpu-unlock-diag}
KVER=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "run as root: sudo bash $0" >&2; exit 1; }
mkdir -p "$OUT"
REPORT=$OUT/report.txt

section() { printf '\n===== %s =====\n' "$*"; }
run() { printf '\n$ %s\n' "$*"; "$@" 2>&1; }

{
section "System"
run uname -a
run cat /etc/version
run cat /proc/cmdline
run cat /sys/kernel/security/lockdown
run cat /proc/sys/kernel/tainted
run cat /sys/module/module/parameters/sig_enforce
for p in / /usr /etc /data /root /home /var; do run findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS -T "$p"; done
run zfs list -r -o name,mountpoint,readonly,used "$(findmnt -no SOURCE /)"
run ls -la /etc/systemd/system /etc/modprobe.d

section "Kernel config"
grep -E '^(# )?CONFIG_(MODULE_SIG[A-Z_]*|MODVERSIONS|MODULE_COMPRESS[A-Z_]*|APERTURE_HELPERS|FW_LOADER[A-Z_]*|EXTRA_FIRMWARE_DIR|PCI|CC_VERSION_TEXT|LOCALVERSION|SECURITY_LOADPIN[A-Z_]*|MODULE_ALLOW_BTF_MISMATCH|DEBUG_INFO_BTF_MODULES)[ =]' \
	"/boot/config-$KVER" 2>&1
run cat /sys/module/firmware_class/parameters/path

section "GPU"
run lspci -D -nn -d 10de:
for GPU in $(lspci -D -n -d 10de::0300 | cut -d' ' -f1); do
	run lspci -vvv -nnk -s "$GPU"
	run ls -l "/sys/bus/pci/devices/$GPU/driver"
	run cat "/sys/bus/pci/devices/$GPU/boot_vga"
	run cat "/sys/bus/pci/devices/$GPU/power/control"
done
run nvidia-smi
run nvidia-smi -q -d CLOCK,POWER,PERFORMANCE,TEMPERATURE
run cat /proc/fb

section "NVIDIA driver and how it gets loaded"
run lsmod
run modinfo nvidia
run cat /sys/module/nvidia/version
run systemd-sysext status
run ls -la /run/extensions /var/lib/extensions /etc/extensions /usr/share/truenas/sysext-extensions
run grep -rn nvidia /etc/modprobe.d /usr/lib/modprobe.d /etc/modules-load.d /usr/lib/modules-load.d /etc/modules
run grep -rln nvidia /usr/lib/udev/rules.d /etc/udev/rules.d
run systemctl list-unit-files 'ix-*' '*nvidia*' 'systemd-sysext*'
run systemctl cat ix-preinit.service ix-postinit.service ix-zfs.service systemd-sysext.service middlewared.service
run grep -rn -E 'modprobe|sysext|insmod' /usr/lib/python3/dist-packages/middlewared/plugins/nvidia* /usr/lib/python3/dist-packages/middlewared/plugins/docker/
run grep -rln -iE 'nvidia' /usr/lib/python3/dist-packages/middlewared/plugins

section "Boot timeline (monotonic)"
run journalctl --list-boots --no-pager
run systemctl show -p ActiveEnterTimestampMonotonic -p ExecMainStartTimestampMonotonic middlewared.service ix-zfs.service ix-preinit.service
journalctl -b -o short-monotonic --no-pager 2>&1 | grep -iE 'nvidia|sysext|gpu_unlock|simpledrm|efifb|fbcon|vfio|zfs|pool|ix-preinit|ix-postinit|ix-zfs|middlewared|Reached target|Xid' | head -400

section "TrueNAS configuration"
# Only the fields that matter here; the full configs hold unrelated settings.
for key in isolated_gpu_pci_ids kernel_extra_options; do
	printf '%s: ' "$key"; midclt call system.advanced.config | python3 -c "import json,sys; print(json.load(sys.stdin)['$key'])"
done
printf 'docker nvidia: '; midclt call docker.config | python3 -c "import json,sys; print(json.load(sys.stdin)['nvidia'])"
run midclt call initshutdownscript.query

section "Kernel headers"
run ls -la "/lib/modules/$KVER/build" "/lib/modules/$KVER/build/"
run find -L "/lib/modules/$KVER/build" -maxdepth 1 -type l -printf '%p -> %l\n'
run file "/lib/modules/$KVER/build/scripts/mod/modpost" "/lib/modules/$KVER/build/scripts/basic/fixdep"
} >"$REPORT"

# Dereference symlinks so the tarball holds the real files.
tar -C "$(realpath "/lib/modules/$KVER/build")" -chzf "$OUT/kernel-headers.tar.gz" . 2>>"$REPORT"
cp "/boot/config-$KVER" "$OUT/config-$KVER"
sha256sum "$OUT"/* >"$OUT/SHA256SUMS"
echo "Done. Copy $OUT to the build machine and run:"
echo "  packaging/truenas/build.sh <copied dir>"
