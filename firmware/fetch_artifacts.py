#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch the NVIDIA inputs without executing or installing them."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import tempfile
import urllib.request

INPUTS = (
    {
        "filename": "NVIDIA-kernel-module-source-610.43.03.tar.xz",
        "url": "https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/"
               "NVIDIA-kernel-module-source-610.43.03.tar.xz",
        "sha256": "7e118923c7a23edc36114d63273a46e3e04e9af98695a42203e7ac2dfe9fc1dc",
    },
    {
        "filename": "NVIDIA-Linux-x86_64-610.43.03-no-compat32.run",
        "url": "https://download.nvidia.com/XFree86/Linux-x86_64/610.43.03/"
               "NVIDIA-Linux-x86_64-610.43.03-no-compat32.run",
        "sha256": "e084451db730a6df29d3d74f655749c4d236e3c945f7d94b5205e84318fe2f90",
    },
)


def verify(entry: dict, path: Path) -> None:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    if digest.hexdigest() != entry["sha256"]:
        raise ValueError(f"sha256 mismatch for {entry['filename']}: got {digest.hexdigest()}")


def fetch(entry: dict, output: Path, offline: bool = False) -> Path:
    output.mkdir(parents=True, exist_ok=True)
    destination = output / entry["filename"]
    if destination.exists():
        verify(entry, destination)
        return destination
    if offline:
        raise FileNotFoundError(f"missing cached artifact: {destination}")
    temporary = None
    try:
        request = urllib.request.Request(entry["url"], headers={"User-Agent": "gpu-unlock-artifacts/0.1.0"})
        with urllib.request.urlopen(request, timeout=60) as response:
            if not response.url.startswith("https://"):
                raise ValueError("refusing non-HTTPS redirect")
            with tempfile.NamedTemporaryFile(dir=output, prefix=".download-", delete=False) as stream:
                temporary = Path(stream.name)
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > 1024 * 1024 * 1024:
                        raise ValueError("artifact exceeds 1 GiB download limit")
                    stream.write(block)
                stream.flush()
                os.fsync(stream.fileno())
        verify(entry, temporary)
        temporary.chmod(0o644)
        temporary.replace(destination)
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="cache directory (for example scratchpad/artifacts)")
    parser.add_argument("--offline", action="store_true", help="use cached inputs without network")
    args = parser.parse_args()
    try:
        for entry in INPUTS:
            print(fetch(entry, args.output, args.offline))
    except (OSError, ValueError) as error:
        parser.exit(1, f"fetch failed: {error}\n")


if __name__ == "__main__":
    main()
