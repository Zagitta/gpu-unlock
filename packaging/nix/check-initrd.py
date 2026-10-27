#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the actual built initrd without loading a module or accessing a GPU."""
import hashlib
import configparser
import os
import re
import shlex
from collections import deque
from pathlib import Path
from pathlib import PurePosixPath
import subprocess
import sys

root, firmware = map(Path, sys.argv[1:3])
root = root.resolve()
kernel = sys.argv[3]


def rooted(name, follow_last=True):
    """Follow an unpacked initrd's absolute links inside its root, never the host."""
    pending = deque(PurePosixPath(name).parts)
    current = root
    links = 0
    while pending:
        part = pending.popleft()
        if part == "/":
            current = root
            continue
        if part == "..":
            current = current.parent
            assert current.is_relative_to(root), name
            continue
        candidate = current / part
        if candidate.is_symlink() and (pending or follow_last):
            links += 1
            assert links <= 40, name
            target = candidate.readlink()
            if target.is_absolute():
                current = root
            pending.extendleft(reversed(PurePosixPath(target).parts))
        else:
            current = candidate
    return current


modules = [p for p in root.glob(f"nix/store/*/lib/modules/{kernel}/extra/gpu_unlock.ko*")
           if p.name in ("gpu_unlock.ko", "gpu_unlock.ko.zst", "gpu_unlock.ko.xz")]
assert len(modules) == 1, modules
assert subprocess.check_output(["modinfo", "-F", "vermagic", str(modules[0])], text=True).startswith(kernel + " ")
required = set(subprocess.check_output(["modinfo", "-F", "firmware", str(modules[0])], text=True).splitlines())
assert required, "module declares no firmware"
for name in required:
    installed = firmware / "lib/firmware" / name
    digest = hashlib.sha256(installed.read_bytes()).hexdigest()
    copies = [p for p in root.glob(f"nix/store/*/lib/firmware/{name}*")
              if p.name in (Path(name).name, Path(name).name + ".zst")]
    assert len(copies) == 1, (name, copies)
    data = (subprocess.check_output(["zstd", "-dc", str(copies[0])])
            if copies[0].suffix == ".zst" else copies[0].read_bytes())
    assert hashlib.sha256(data).hexdigest() == digest, name
if sys.argv[4] == "systemd":
    unit = rooted("etc/systemd/system/gpu-unlock.service").read_text()
    # systemd permits repeated assignments, including generated Environment=.
    parsed = configparser.ConfigParser(interpolation=None, strict=False)
    parsed.read_string(unit)
    before = parsed["Unit"]["Before"].split()
    for dependency in ("systemd-modules-load.service", "systemd-udevd.service", "systemd-udev-trigger.service"):
        assert dependency in before, dependency
    assert rooted("etc/systemd/system/sysinit.target.wants/gpu-unlock.service", follow_last=False).is_symlink()
    assert parsed["Unit"]["DefaultDependencies"].lower() == "false"
    assert parsed["Service"]["Type"] == "oneshot"
    command = shlex.split(parsed["Service"]["ExecStart"])
    assert command[1:] == ["gpu_unlock"]
    assert os.access(rooted(command[0]), os.X_OK), command
    Path("verified-initrd-ordering.txt").write_text(unit)
else:
    init = rooted("init").read_text()
    loop = re.search(r"for i in ([^\n;]*\bgpu_unlock\b[^\n;]*); do", init)
    assert loop, "gpu_unlock missing from synchronous stage-1 loading loop"
    assert loop.start() < init.index("systemd-udevd --daemon"), "module loaded after udev"
    Path("verified-initrd-ordering.txt").write_text(loop.group(0) + "\n")
