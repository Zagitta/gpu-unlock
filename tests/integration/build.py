# SPDX-License-Identifier: GPL-2.0-only
"""Build the runner shared object from the working tree.

The runner links the real driver sources, the kernel shim and the native
device library into one object. Python loads it with ctypes and calls
gd_run_transaction once per transaction.
"""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DRIVER = ROOT / "kernel" / "gpu_unlock"

SOURCES = [
    HERE / "runner.c",
    HERE / "shim" / "shim.c",
    HERE / "device" / "gd.c",
    HERE / "device" / "aes.c",
    HERE / "device" / "wpr2.c",
    HERE / "device" / "services.c",
    DRIVER / "firmware_610.c",
    DRIVER / "unlock_610.c",
    DRIVER / "falcon.c",
    DRIVER / "vbios_610.c",
    DRIVER / "layout_610.c",
    DRIVER / "prep_610.c",
    DRIVER / "payload.c",
    DRIVER / "recipes.c",
    DRIVER / "gfw_eval.c",
]

FLAGS = [
    "-std=gnu11", "-O2", "-g", "-fPIC", "-shared", "-pthread",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
    "-I" + str(HERE / "shim"),
    "-I" + str(ROOT),
    "-DGPU_UNLOCK_FWSEC_APP=0x45",
]


def build(output: Path | None = None, *, sanitize: bool = False,
          verbose: bool = False) -> Path:
    output = Path(output or ROOT / "build" / "integration")
    output.mkdir(parents=True, exist_ok=True)
    library = output / "libgdrunner.so"
    flags = list(FLAGS)
    if sanitize:
        flags[1] = "-O1"
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command = shlex.split(os.environ.get("CC", "cc")) + flags + [
        str(source) for source in SOURCES
    ] + ["-o", str(library)]
    if verbose:
        print("building:", shlex.join(command), file=sys.stderr)
    subprocess.run(command, check=True, cwd=ROOT)
    return library


if __name__ == "__main__":
    print(build(verbose=bool(os.environ.get("GPU_UNLOCK_DIAG"))))
