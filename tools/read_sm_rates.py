#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read current SM issue-rate selectors using NVIDIA's public RM getter.

No BAR mapping, register write, firmware upload, reset, CUDA context or CUDA kernel.
Allocates only temporary RM client/device/subdevice handles, closed on exit.
Requires one GPU with the specified UUID to avoid device-instance ambiguity.
ABI: NVIDIA open-gpu-kernel-modules 610.43.03; source-checked against 595.112.07.
"""
import argparse
import ctypes as C
import json
import os
import subprocess

U32, U64 = C.c_uint32, C.c_uint64
GET_SM_RATES = 0x20801230


class Alloc(C.Structure):
    _fields_ = [(name, U32) for name in ("root", "parent", "new", "cls")] + [
        ("params", U64), ("rights", U64), ("size", U32),
        ("flags", U32), ("status", U32)]


class Control(C.Structure):
    _fields_ = [(name, U32) for name in ("client", "obj", "cmd", "flags")] + [
        ("params", U64), ("size", U32), ("status", U32)]


class Device(C.Structure):
    _fields_ = [(name, U32) for name in ("device", "share", "target_client", "target_device", "flags")] + [
        ("va_size", U64), ("va_start", U64), ("va_limit", U64), ("va_mode", U32)]


RATE_FIELDS = ("imla0", "fmla16", "dp", "fmla32", "ffma", "imla1", "imla2", "imla3", "imla4")
RATE_MAX = dict(zip(RATE_FIELDS, (6, 5, 1, 5, 5, 6, 6, 6, 6)))


class Rates(C.Structure):
    _fields_ = [("route_flags", U32), ("padding", U32), ("route", U64)] + [
        (name, C.c_uint8) for name in RATE_FIELDS]


assert C.sizeof(Alloc) == 48 and C.sizeof(Control) == 32
assert C.sizeof(Device) == 56 and C.sizeof(Rates) == 32
assert Rates.imla0.offset == 16 and Rates.imla4.offset == 24


def ioctl_number(number, structure):
    return (3 << 30) | (C.sizeof(structure) << 16) | (ord("F") << 8) | number


def call_ioctl(fd, number, structure, ioctl):
    buffer = bytearray(bytes(structure))
    ioctl(fd, number, buffer, True)
    C.memmove(C.addressof(structure), bytes(buffer), len(buffer))
    if structure.status:
        raise RuntimeError(f"RM request failed with status 0x{structure.status:x}")


def gpu_identity(executable="nvidia-smi"):
    result = subprocess.run(
        [executable, "--query-gpu=uuid,name,pci.bus_id,driver_version,power.limit,clocks.gr,clocks.mem",
         "--format=csv,noheader,nounits"], check=True, capture_output=True, text=True, timeout=8)
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def read_rates(expected_uuid, ioctl=None, executable="nvidia-smi"):
    identity = gpu_identity(executable)
    if len(identity) != 1 or identity[0].split(",", 1)[0].strip() != expected_uuid:
        raise RuntimeError("expected one GPU with the exact supplied UUID")
    if ioctl is None:
        import fcntl
        ioctl = fcntl.ioctl
    descriptors = []
    try:
        open_flags = os.O_RDWR | getattr(os, "O_CLOEXEC", 0)
        ctl = os.open("/dev/nvidiactl", open_flags)
        descriptors.append(ctl)
        descriptors.append(os.open("/dev/nvidia0", open_flags))

        def allocate(root, parent, cls, params=None):
            request = Alloc(root=root, parent=parent, cls=cls,
                            params=C.addressof(params) if params is not None else 0,
                            size=C.sizeof(params) if params is not None else 0)
            call_ioctl(ctl, ioctl_number(0x2B, Alloc), request, ioctl)
            if not request.new:
                raise RuntimeError("RM returned an empty object handle")
            return request.new

        client = allocate(0, 0, 0x41)
        device = allocate(client, client, 0x80, Device())
        subdevice = allocate(client, device, 0x2080, U32(0))
        rates = Rates()
        request = Control(client=client, obj=subdevice, cmd=GET_SM_RATES,
                          params=C.addressof(rates), size=C.sizeof(rates))
        call_ioctl(ctl, ioctl_number(0x2A, Control), request, ioctl)
        values = {name: getattr(rates, name) for name in RATE_FIELDS}
        if any(value > RATE_MAX[name] for name, value in values.items()):
            raise RuntimeError("driver returned an unknown issue-rate encoding")
        return {"gpu": identity[0], "command": f"0x{GET_SM_RATES:x}",
                "rates": values,
                "meaning": "0=full; dp=1 means reduced; other fields: 1=half, 2=quarter, higher values are further reductions",
                "hardware_changes": False}
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-uuid", required=True)
    parser.add_argument("--nvidia-smi", default="nvidia-smi",
                        help="path to the nvidia-smi executable")
    args = parser.parse_args()
    print(json.dumps(read_rates(args.expect_uuid,
                                executable=args.nvidia_smi)))


if __name__ == "__main__":
    main()
