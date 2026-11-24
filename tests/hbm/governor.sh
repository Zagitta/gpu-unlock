#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the production HBM governor policy with simulated memory controllers.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/linux"
for header in delay io stop_machine string; do
	: > "$tmp/linux/$header.h"
done
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$tmp" -I"$repo/kernel/gpu_unlock" \
	"$repo/tests/hbm/governor.c" -o "$tmp/governor"
"$tmp/governor"
