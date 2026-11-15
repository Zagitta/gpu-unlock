#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract the GSP firmware and NVIDIA license from the driver installer."""

from __future__ import annotations

import argparse
import re
import subprocess
import tarfile
from pathlib import Path

FIRMWARE = (
    "firmware/gsp_ga10x.bin",
    "firmware/gsp_tu10x.bin",
)
NOTICE = "LICENSE"
VERSION = "610.43.03"
HEADER_BYTES = 1 << 20


def payload_offset(installer: Path) -> int:
    """Offset of the makeself payload after its shell header."""
    with installer.open("rb") as stream:
        header = stream.read(HEADER_BYTES)
    version = re.search(rb"^version_string=([0-9.]+)$", header, re.M)
    skip = re.search(rb"^skip=(\d+)$", header, re.M)
    if not version or not skip:
        raise ValueError("not an NVIDIA makeself installer")
    if version[1].decode() != VERSION:
        raise ValueError(f"installer version is {version[1].decode()}, expected {VERSION}")
    # The installer extracts with `tail -n +$skip`, so the payload starts
    # after skip - 1 header lines.
    offset = 0
    for _ in range(int(skip[1]) - 1):
        newline = header.find(b"\n", offset)
        if newline < 0:
            raise ValueError("installer header is shorter than its skip count")
        offset = newline + 1
    return offset


def read_members(archive: tarfile.TarFile, names: tuple[str, ...]) -> dict[str, bytes]:
    wanted = set(names)
    result = {}
    for member in archive:
        if member.name not in wanted:
            continue
        if member.name in result:
            raise ValueError(f"duplicate installer member: {member.name}")
        if not member.isfile() or member.size > 128 * 1024 * 1024:
            raise ValueError(f"not a regular file in installer: {member.name}")
        stream = archive.extractfile(member)
        if stream is None:
            raise ValueError(f"could not read installer member: {member.name}")
        result[member.name] = stream.read()
    missing = sorted(wanted - result.keys())
    if missing:
        raise ValueError(f"missing installer member: {', '.join(missing)}")
    return result


def extract_members(installer: Path, names: tuple[str, ...]) -> dict[str, bytes]:
    offset = payload_offset(installer)
    with installer.open("rb") as stream:
        stream.seek(offset)
        process = subprocess.Popen(["zstd", "-dc"], stdin=stream, stdout=subprocess.PIPE)
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
                result = read_members(archive, names)
            if process.wait() != 0:
                raise ValueError("zstd could not read the installer payload")
            return result
        finally:
            if process.stdout:
                process.stdout.close()
            if process.poll() is None:
                process.kill()
                process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("installer", type=Path, help="NVIDIA-Linux-x86_64-610.43.03-no-compat32.run")
    parser.add_argument("output", type=Path, help="directory for extracted firmware")
    args = parser.parse_args()

    members = extract_members(args.installer, (*FIRMWARE, NOTICE))
    args.output.mkdir(parents=True, exist_ok=True)
    for path in FIRMWARE:
        (args.output / Path(path).name).write_bytes(members[path])
    (args.output / "NVIDIA-LICENSE").write_bytes(members[NOTICE])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"extraction failed: {error}")
