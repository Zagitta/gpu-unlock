# SPDX-License-Identifier: GPL-2.0-only
"""Hook parity for the device model.

booter.toml is the source of the register inventory. The generated header in
device/booter_regs.h must match its resets and masks, and every non-storage
operation must have a compiled counterpart in gd.c. Unsupported operations
fail the parity check instead of silently drifting.
"""
from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOML = ROOT / "emulator" / "hw" / "booter.toml"
HEADER = HERE / "device" / "booter_regs.h"
DEVICE = HERE / "device" / "gd.c"

# These are the only non-storage operations in booter.toml.
COMPILED_HOOKS = {0x404, 0x700}


def inventory():
    with TOML.open("rb") as file:
        data = tomllib.load(file)
    rows = {"bar0": [], "io": []}
    special = []
    for name, reg in data.get("reg", {}).items():
        space = reg.get("space", "io")
        rows[space].append((reg["addr"], reg.get("reset", 0), reg.get("mask", 0xFFFFFFFF)))
        known = {"space", "addr", "reset", "mask", "w1c", "plm", "read_levels",
                 "write_levels", "flr_reset", "denied_read", "host", "io"}
        if set(reg) - known:
            special.append((name, reg))
    for space in rows:
        rows[space].sort()
    return rows, special


def header_rows(space: str):
    text = HEADER.read_text()
    start = text.index("gd_%s_defaults[]" % space)
    body = text[start:text.index("};", start)]
    return [(int(a, 16), int(v, 16), int(m, 16))
            for a, v, m in re.findall(r"\{ 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u \}", body)]


def check() -> list[str]:
    failures = []
    rows, special = inventory()
    for space in ("bar0", "io"):
        if header_rows(space) != rows[space]:
            failures.append(f"{space} reset/mask inventory drifted from booter.toml")
    addresses = {reg["addr"] for _, reg in special}
    if addresses != COMPILED_HOOKS:
        failures.append(f"TOML hooks {sorted(addresses)} != compiled {sorted(COMPILED_HOOKS)}")
    device = DEVICE.read_text()
    for address in sorted(COMPILED_HOOKS):
        if "0x%x" % address not in device:
            failures.append(f"compiled hook 0x{address:x} missing from gd.c")
    return failures


if __name__ == "__main__":
    problems = check()
    for problem in problems:
        print(problem, file=sys.stderr)
    raise SystemExit(1 if problems else 0)
