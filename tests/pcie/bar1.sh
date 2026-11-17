#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the production resize code with simulated PCI resource allocation.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
sed -n '/^#define GU_BAR1 /,$p' "$repo/kernel/gpu_unlock/pcie.c" > "$tmp/bar1-code.c"
for api in modern legacy; do
	flags=
	[ "$api" != modern ] || flags=-DGU_RESIZE_EXCLUDE_BARS
	# shellcheck disable=SC2086
	${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -pthread $flags -I"$tmp" \
		"$repo/tests/pcie/bar1.c" -o "$tmp/bar1"
	"$tmp/bar1"
done
