# SPDX-License-Identifier: GPL-2.0-only
"""Pre-decode the debug FWSEC IMEM with the Python reference decoder.

The device still loads and decrypts the image itself. This table is input
data for the C interpreter, so no Python runs after the transaction starts.
"""
from __future__ import annotations

import ctypes as C
import struct
from pathlib import Path

from emulator.falcon_emu.decode import _decode

OPS = {
    "fallback": 0, "mov": 1, "sethi": 2, "add": 3, "adc": 4, "sub": 5,
    "sbb": 6, "cmpu": 7, "cmps": 8, "cmp": 9, "shl": 10, "shr": 11,
    "sar": 12, "shlc": 13, "shrc": 14, "not": 15, "neg": 16, "hswap": 17,
    "clear": 18, "setf": 19, "ld": 20, "st": 21, "mulu": 22, "muls": 23,
    "sext": 24, "extr": 25, "extrs": 26, "ins": 27, "and": 28, "or": 29,
    "xor": 30, "xbit": 31, "xbitf": 32, "bset": 33, "bclr": 34, "btgl": 35,
    "div": 36, "mod": 37, "bra": 38, "brcmp": 39, "jmp": 40, "addsp": 41,
    "nop": 42, "movsr": 43, "movrs": 44, "call": 45, "ret": 46, "push": 47,
    "pop": 48, "mpush": 49, "mpop": 50, "mpopret": 51, "mpopadd": 52,
    "mpopaddret": 53, "lbra": 65, "lcall": 66, "exit": 63, "trap": 64,
    "iord": 60, "iords": 61, "iowr": 62, "iowrs": 62,
    "sleep": 67, "xdwait": 78, "xdfence": 79, "xcwait": 80,
    "csecret": 70, "cenc": 71, "cxor": 72, "crnd": 73, "ckeyreg": 74,
    "cmov": 75, "xdst": 76, "xdld": 77, "cxsin": 81, "cxsout": 82,
    "vtlb": 83, "itlb": 84, "ptlb": 85, "cs0begin": 86, "cs0exec": 87,
    "csigclr": 88, "bsetf": 89, "bclrf": 90, "btglf": 91, "setp": 92, "cxset": 93,
    "iret": 94, "xcld": 95,
    "cadd": 96, "cand": 97, "crev": 98, "cgfmul": 99, "cchmod": 100,
    "ckexp": 101, "ckrexp": 102, "cdec": 103, "csigcmp": 104, "csigenc": 105,
    "cs1begin": 106, "cs1exec": 107,
    "cicmov": 120, "cixsin": 121, "cixsout": 122, "cicrnd": 123,
    "cics0begin": 124, "cics0exec": 125, "cics1begin": 126, "cics1exec": 127,
    "cichmod": 128, "cixor": 129, "cicadd": 130, "cicand": 131,
    "cicrev": 132, "cicgfmul": 133, "cicsecret": 134, "cikeyreg": 135,
    "cikexp": 136, "cikrexp": 137, "cicenc": 138, "cidec": 139,
    "cisigcmp": 140, "cisigenc": 141, "cisigclr": 142,
}


class Info(C.Structure):
    _fields_ = [("version", C.c_uint32), ("image_off", C.c_uint32),
                ("image_size", C.c_uint32), ("data_off", C.c_uint32),
                ("imem_size", C.c_uint32), ("dmem_size", C.c_uint32),
                ("interface_off", C.c_uint32)]


def build(library: Path, rom: Path, key: bytes, output: Path) -> int:
    lib = C.CDLL(str(library))
    lib.gd_fwsec_extract.argtypes = [C.c_char_p, C.POINTER(Info)]
    info = Info()
    if lib.gd_fwsec_extract(str(rom).encode(), C.byref(info)) != 0:
        raise SystemExit("code table: FWSEC descriptor not found")
    image = bytearray(Path(rom).read_bytes()[info.image_off:info.image_off + info.image_size])
    if len(key) != 16:
        raise SystemExit("code table: key must be 16 bytes")
    imem = image[:info.imem_size]
    buffer = (C.c_ubyte * len(imem)).from_buffer(imem)
    if lib.gd_aes_decrypt_buffer(key, buffer, len(imem)) != 0:
        raise SystemExit("code table: AES failed")
    return _write_table(imem, output, 0)


def _write_table(imem: bytes | bytearray, output: Path, base: int) -> int:
    records = []
    pc = base
    while pc < base + len(imem):
        rel = pc - base
        ins = _decode(pc, bytes(imem[rel:rel + 6]).ljust(6, b"\0"))
        if ins.op == "invalid":
            break
        target = ins.mem if isinstance(ins.mem, int) else 0
        if ins.op in ("bra", "jmp", "lbra", "lcall", "call") and target == 0:
            target = ins.a if isinstance(ins.a, int) else 0
        mem = ins.mem if hasattr(ins.mem, "base") else None
        mbase = 16 if mem is not None and mem.base == "sp" else (mem.base if mem is not None else 0)
        idx = mem.idx if mem is not None and mem.idx is not None else -1
        off = mem.off if mem is not None else 0
        scale = mem.scale if mem is not None else 1
        space = 1 if mem is not None and mem.space == "I" else 0
        records.append(struct.pack(
            "<IIiIIIiiiBBBBBB2x",
            pc & 0xffffffff, target & 0xffffffff, off,
            (ins.a & 0xffffffff) if isinstance(ins.a, int) else 0,
            (ins.b & 0xffffffff) if isinstance(ins.b, int) else 0,
            (ins.d & 0xffffffff) if isinstance(ins.d, int) else 0,
            mbase, idx, scale,
            ins.length, OPS.get(ins.op, 0), ins.sz, 1 if ins.sized else 0,
            1 if ins.bimm else 0, space))
        pc += ins.length
    output.write_bytes(struct.pack("<4sII", b"GDC1", len(records), len(imem)) + b"".join(records))
    return len(records)


def build_booter(library: Path, bundle: Path, key: bytes, output: Path) -> int:
    """Decode the SEC2 Booter application image, not the FWSEC image."""
    data = Path(bundle).read_bytes()
    if len(data) < 80:
        raise SystemExit("code table: short Booter bundle")
    fields = struct.unpack_from("<14I", data, 8)
    app_code_off, app_code_size = fields[5], fields[6]
    image = data[72:]
    if app_code_off + app_code_size > len(image):
        raise SystemExit("code table: Booter app code outside image")
    if len(key) != 16:
        raise SystemExit("code table: key must be 16 bytes")
    code = bytearray(image[app_code_off:app_code_off + app_code_size])
    lib = C.CDLL(str(library))
    lib.gd_aes_decrypt_buffer.argtypes = [C.c_char_p, C.c_void_p, C.c_size_t]
    buffer = (C.c_ubyte * len(code)).from_buffer(code)
    if lib.gd_aes_decrypt_buffer(key, buffer, len(code)) != 0:
        raise SystemExit("code table: Booter AES decrypt failed")
    count = _write_table(code, output, app_code_off)
    if not count:
        raise SystemExit("code table: Booter code is not decodable; install the external debug image")
    return count
