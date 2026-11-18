#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$repo/kernel/gpu_unlock" \
	"$repo/tests/vbios/power.c" "$repo/kernel/gpu_unlock/vbios.c" -o "$tmp/power"
"$tmp/power"
