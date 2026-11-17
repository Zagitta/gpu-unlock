#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Back up, provision missing EEN/ECC objects, and verify using NVFlash IFR files.

Run from Linux with the GPU unbound and the NVIDIA modules unloaded. Store the
backup on persistent storage, particularly when using a live environment.
Supports the Ampere and Ada device IDs listed in kernel/gpu_unlock/recipes.c.
"""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile


ROM_SIZE = 112
DIRECTORY = range(14, 109, 5)
MAX_IFR_SIZE = 0x20000
# Keep in sync with the Ampere and Ada entries in recipes.c.
SUPPORTED_DEVICES = {
    # GA100: CMP 170HX 8GB/10GB
    0x20C2, 0x2082,
    # GA102
    0x2200, 0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220A,
    0x220D, 0x2216, 0x222B, 0x222F,
    # GA103
    0x2414,
    # GA104
    0x2480, 0x2482, 0x2483, 0x2484, 0x2486, 0x2487, 0x2488, 0x2489,
    0x248A, 0x248C, 0x248D, 0x248E, 0x249C, 0x249D, 0x249F, 0x24A0,
    0x24A4, 0x24AC, 0x24AD, 0x24AF, 0x24BF, 0x24C0, 0x24C7, 0x24C8,
    0x24C9, 0x24DC, 0x24DD, 0x24DF, 0x24E0,
    # GA106
    0x2501, 0x2503, 0x2504, 0x2505, 0x2507, 0x2508, 0x2509, 0x252F,
    0x2544,
    # AD102
    0x2684, 0x2685, 0x2689,
    # AD103
    0x2702, 0x2704, 0x2705, 0x2709, 0x2757,
    # AD104
    0x2782, 0x2783, 0x2786,
    # AD106
    0x2803, 0x2805,
    # AD107
    0x2882, 0x28A0, 0x28E0, 0x28E1,
}
# In NVFlash's flat IFR representation byte 7 carries file attributes, rather
# than the legacy object checksum: ECC is writable, EEN also ignores checksum.
# These match native exports and the validated NVIDIA filesystem round trips.
DEFAULTS = {
    b"EEN": b"EEN\x01\x00\x70\x00\x03" + b"\xff" * 104,
    b"ECC": b"ECC\x06\x10\xe0\x4d\x01" + bytes(19928),
}


def parse_ifr(data):
    """Return complete objects, rejecting ambiguous directories and bounds."""
    if not ROM_SIZE <= len(data) <= MAX_IFR_SIZE:
        raise ValueError("Invalid IFR length")
    if data[:8] != b"ROM\x01\x02\x70\x00\x00":
        raise ValueError("Expected a native NVFlash IFR with ROM directory v1.2")
    objects = {b"ROM": data[:ROM_SIZE]}
    spans = [(0, ROM_SIZE)]
    empty = False
    for slot in DIRECTORY:
        entry = data[slot:slot + 5]
        if entry == bytes(5):
            empty = True
            continue
        tag, offset = struct.unpack("<3sH", entry)
        if empty or not re.fullmatch(rb"[A-Z0-9]{3}", tag) or tag in objects:
            raise ValueError("Invalid, duplicate, or noncontiguous IFR directory entry")
        if offset < ROM_SIZE or offset + 8 > len(data):
            raise ValueError(f"Invalid offset for {tag!r}")
        size = struct.unpack_from("<H", data, offset + 5)[0]
        end = offset + size
        if data[offset:offset + 3] != tag or size < 8 or end > len(data):
            raise ValueError(f"Invalid object header/length for {tag!r}")
        if any(offset < stop and end > start for start, stop in spans):
            raise ValueError(f"Overlapping object {tag!r}")
        spans.append((offset, end))
        objects[tag] = data[offset:end]
    return objects


def enable_een(een):
    if len(een) != 112 or een[:8] != DEFAULTS[b"EEN"][:8]:
        raise ValueError("Unvalidated EEN format; refusing to enable ECC")
    # EEN is the parity of the first nonzero dword, not a boolean field.
    # Clear one set bit only when disabled, preserving the journal history.
    for offset in range(8, len(een), 4):
        word = struct.unpack_from("<I", een, offset)[0]
        if not word:
            continue
        if word.bit_count() & 1:
            return een
        enabled = bytearray(een)
        struct.pack_into("<I", enabled, offset, word & (word - 1))
        return bytes(enabled)
    raise ValueError("EEN journal is exhausted; refusing to enable ECC")


def patch_ifr(data, enable_ecc=False):
    objects = parse_ifr(data)
    missing = [tag for tag in DEFAULTS if tag not in objects]
    if missing and data[8:10] != bytes(2):
        raise ValueError("Unvalidated nonzero ROM totalDataSize; refusing to modify")
    if len(objects) - 1 + len(missing) > len(DIRECTORY):
        raise ValueError("No room in IFR directory for the missing objects")
    patched = bytearray(data)
    for tag in missing:
        offset = len(patched)
        if offset > 0xFFFF or offset + len(DEFAULTS[tag]) > MAX_IFR_SIZE:
            raise ValueError("Patched IFR exceeds native directory/file limits")
        slot = 14 + 5 * (len(objects) - 1)
        patched[slot:slot + 5] = struct.pack("<3sH", tag, offset)
        patched.extend(DEFAULTS[tag])
        objects[tag] = DEFAULTS[tag]
    if enable_ecc:
        een = enable_een(objects[b"EEN"])
        offset = next(struct.unpack_from("<H", patched, slot + 3)[0]
                      for slot in DIRECTORY if patched[slot:slot + 3] == b"EEN")
        patched[offset:offset + len(een)] = een
        objects[b"EEN"] = een
    result = bytes(patched)
    verified = parse_ifr(result)
    if any(verified[tag] != obj for tag, obj in objects.items() if tag != b"ROM"):
        raise ValueError("Internal error: patch changed an existing object")
    return result, [tag.decode("ascii") for tag in missing]


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_file(path, data):
    with path.open("xb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    sync_directory(path.parent)


def seal_export(path):
    data = path.read_bytes()
    parse_ifr(data)
    # NVFlash created this file. Make its contents and directory entry durable
    # before allowing any write to the card.
    with path.open("rb") as saved:
        os.fsync(saved.fileno())
    path.chmod(0o400)
    sync_directory(path.parent)
    return data


def check_device(bdf):
    if not re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\.[0-7]", bdf):
        raise ValueError("Use a full PCI address, for example 0000:c2:00.0")
    device = Path("/sys/bus/pci/devices") / bdf
    vendor = int((device / "vendor").read_text(), 16)
    chip = int((device / "device").read_text(), 16)
    if vendor != 0x10DE or chip not in SUPPORTED_DEVICES:
        raise ValueError(f"Unsupported device {vendor:04x}:{chip:04x}; refusing to flash")
    if (device / "driver").is_symlink():
        raise ValueError(f"{bdf} is bound to a driver; boot with the GPU unbound")
    modules = [line.split()[0] for line in Path("/proc/modules").read_text().splitlines()]
    if any(name == "nvidia" or name.startswith("nvidia_") for name in modules) or Path("/sys/module/nvidia").exists():
        raise ValueError("NVIDIA modules are loaded; boot without the NVIDIA driver")
    return {"bdf": bdf, "vendor": vendor, "device": chip,
            "subsystem_vendor": (device / "subsystem_vendor").read_text().strip(),
            "subsystem_device": (device / "subsystem_device").read_text().strip()}


@contextmanager
def uninterrupted_write():
    # Do not let Ctrl-C or an ordinary termination kill a flash in progress.
    previous = {sig: signal.signal(sig, signal.SIG_IGN)
                for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        yield
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def run_nvflash(executable, bdf, log, *arguments):
    command = [executable, "--pcisegbus", bdf, "--auto", "--noconfirm", *map(str, arguments)]
    with log.open("a", encoding="utf-8") as output:
        output.write("\n$ " + shlex.join(command) + "\n")
        output.flush()
        # Inherit output through the log; no shell, input prompts, timeouts,
        # protection overrides, or automatic retries.
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT, check=False)
        output.flush()
        os.fsync(output.fileno())
    return result.returncode


def provision(executable, bdf, backup_parent, dry_run=False, enable_ecc=False):
    identity = check_device(bdf)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = Path(tempfile.mkdtemp(prefix=f"inforom-{bdf}-{stamp}-", dir=backup_parent)).resolve()
    sync_directory(directory.parent)
    print(f"Backup and NVFlash log: {directory}", flush=True)
    original = directory / "original.ifr"
    candidate = directory / "patched.ifr"
    readback = directory / "readback.ifr"
    log = directory / "nvflash.log"
    if run_nvflash(executable, bdf, log, "--save", original):
        raise RuntimeError(f"NVFlash backup failed; see {log}. Nothing flashed.")
    before = seal_export(original)
    patched, added = patch_ifr(before, enable_ecc=enable_ecc)
    write_file(candidate, patched)
    manifest = {"target": identity, "added": added, "enable_ecc": enable_ecc,
                "nvflash": executable,
                "sha256": {"original.ifr": hashlib.sha256(before).hexdigest(),
                           "patched.ifr": hashlib.sha256(patched).hexdigest()}}
    write_file(directory / "backup.json", (json.dumps(manifest, indent=2) + "\n").encode())
    if patched == before:
        print("No InfoROM changes needed; nothing flashed.")
        return directory
    if added:
        print("Added " + ", ".join(added) + ".", flush=True)
    if enable_ecc:
        print("EEN requests ECC enabled at the next boot.", flush=True)
    else:
        print("Existing objects preserved. New EEN defaults to disabled.", flush=True)
    if dry_run:
        print("Dry run complete; nothing flashed.")
        return directory
    if check_device(bdf) != identity or original.read_bytes() != before or candidate.read_bytes() != patched:
        raise RuntimeError("Device or saved files changed; refusing to flash")
    print("Writing IFR. Do not interrupt or power off. NVFlash output goes to nvflash.log.", flush=True)
    with uninterrupted_write():
        status = run_nvflash(executable, bdf, log, "--flashinforom", candidate)
        # Even a failed command may have written something. Always attempt a
        # readback, without retrying the write or attempting automatic rollback.
        read_status = run_nvflash(executable, bdf, log, "--save", readback)
    if read_status:
        raise RuntimeError(f"Readback failed after write (exit {status}); see {log}. Do not retry blindly.")
    actual = parse_ifr(seal_export(readback))
    expected = parse_ifr(patched)
    differences = sorted(tag.decode() for tag in actual.keys() | expected.keys()
                         if actual.get(tag) != expected.get(tag))
    if status or differences:
        raise RuntimeError(f"Flash not verified: exit {status}, differing objects {differences}; see {log}")
    write_file(directory / "verified.txt", b"Fresh IFR export matches every expected object.\n")
    print("Verified every object against a fresh IFR export. Keep original.ifr and backup.json.")
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    apply = modes.add_parser("apply", help="back up, add missing objects, flash, and verify")
    apply.add_argument("--bdf", required=True, type=str.lower, help="full PCI address of the target GPU")
    apply.add_argument("--backup-dir", required=True, type=Path, help="existing directory on persistent storage")
    apply.add_argument("--nvflash", default="nvflash", help="NVFlash executable (default: PATH)")
    apply.add_argument("--dry-run", action="store_true", help="export backup and patch, without flashing")
    offline = modes.add_parser("patch", help="patch an existing IFR file without GPU access")
    offline.add_argument("input", type=Path)
    offline.add_argument("output", type=Path)
    for command in (apply, offline):
        command.add_argument("--enable-ecc", action="store_true",
                             help="set EEN to request ECC enabled at the next boot")
    args = parser.parse_args()
    try:
        if args.mode == "patch":
            data, added = patch_ifr(args.input.read_bytes(), enable_ecc=args.enable_ecc)
            write_file(args.output, data)
            print("Added: " + (", ".join(added) or "nothing (already provisioned)"))
            if args.enable_ecc:
                print("EEN requests ECC enabled at the next boot.")
        else:
            if os.geteuid() != 0:
                raise ValueError("The apply command requires root")
            executable = shutil.which(args.nvflash)
            if executable is None:
                raise ValueError("NVFlash executable not found")
            if not args.backup_dir.is_dir():
                raise ValueError("--backup-dir must be an existing persistent directory")
            # Serialize this helper across all GPUs/processes. Do not unlink a
            # lock file, which would allow concurrent locks on different inodes.
            with open("/run/lock/gpu-unlock-inforom.lock", "a") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                provision(executable, args.bdf, args.backup_dir,
                          dry_run=args.dry_run, enable_ecc=args.enable_ecc)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
