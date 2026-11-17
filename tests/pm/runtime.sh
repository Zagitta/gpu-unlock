#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the actual PM implementation with a simulated PCI/PM core.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/linux/sched"
for header in completion device err interrupt msi pci pm pm_domain pm_runtime slab string sched/mm; do
	: > "$tmp/linux/$header.h"
done
for config in modern legacy no-msi; do
	case "$config" in
	modern) flags='-DCONFIG_PCI_MSI -DGU_MSI_GET_VIRQ' ;;
	legacy) flags='-DCONFIG_PCI_MSI' ;;
	no-msi) flags= ;;
	esac
	# shellcheck disable=SC2086
	${CC:-cc} -std=gnu11 -Wall -Wextra -Werror $flags -I"$tmp" \
		"$repo/tests/pm/runtime.c" -o "$tmp/runtime"
	"$tmp/runtime"
done
