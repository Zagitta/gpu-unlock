#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the Booter and GSP boot assets from the NVIDIA 610 generated source."""
from __future__ import annotations

import argparse
import zlib
import re
import struct
import sys
from pathlib import Path

MAGIC = 0x314C5442

# Generated NVIDIA sources this script reads.
SOURCES = (
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.c',
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_GA100.c',
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_GA102.c',
    'g_bindata_kgspGetBinArchiveBooterLoadUcode_TU102.c',
    'g_bindata_kgspGetBinArchiveGspRmBoot_AD102.c',
    'g_bindata_kgspGetBinArchiveGspRmBoot_GA100.c',
    'g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c',
    'g_bindata_kgspGetBinArchiveGspRmBoot_TU102.c',
    'g_bindata_ksec2GetBinArchiveBlUcode_TU102.c',
)



def _array_bytes(text: str, label: str) -> tuple[bytes, bool, int]:
    marker = f'BINDATA_LABEL_{label}_data[]'
    if text.count(marker) != 1:
        raise ValueError(f"missing or duplicate {label}")
    start = text.index(marker)
    window = text[max(0, start - 400):start]
    compression = re.findall(r"COMPRESSION: (YES|NO)", window)
    sizes = re.findall(r"DATA SIZE \(bytes\): (\d+)", window)
    if not compression or not sizes:
        raise ValueError(f"missing storage declaration for {label}")
    expected = int(sizes[-1]); compressed = compression[-1] == 'YES'
    if not 0 < expected <= 16 * 1024 * 1024:
        raise ValueError('asset exceeds extraction limit')
    declaration = re.match(re.escape(marker) + r"\s*=\s*\{([^}]*)\};", text[start:], re.S)
    if not declaration:
        raise ValueError('malformed byte-array declaration')
    tokens = declaration[1].strip().rstrip(',').split(',')
    if all(re.fullmatch(r"\s*0x[0-9a-fA-F]{2}\s*", item) for item in tokens):
        raw = bytes(int(item, 16) for item in tokens)
    elif all(re.fullmatch(r"\s*0x[0-9a-fA-F]{1,16}ULL\s*", item) for item in tokens):
        # Newer generated sources (615.71+) store the same stream as NvU64
        # words instead of single bytes (issue #168); restore little-endian.
        raw = b"".join(struct.pack("<Q", int(item.strip()[:-3], 16)) for item in tokens)
    else:
        raise ValueError('non-byte token in generated array')
    declared = expected
    if compressed:
        stored = re.findall(r"COMPRESSED SIZE \(bytes\): (\d+)", window)
        if not stored:
            raise ValueError('compressed storage size mismatch')
        declared = int(stored[-1])
    if len(raw) != declared:
        # NvU64 expansion pads the array up to a multiple of 8 bytes while
        # the declared size stays exact.  Accept only an all-zero tail.
        if len(raw) < declared or any(raw[declared:]):
            raise ValueError('compressed storage size mismatch' if compressed
                             else f"{label}: array does not match the declared size")
        raw = raw[:declared]
    if compressed:
        inflater = zlib.decompressobj(-15)
        data = inflater.decompress(raw, expected + 1)
        # NVIDIA BINDATA stores raw DEFLATE followed by the gzip CRC32/ISIZE
        # trailer (the gzip header is omitted). Validate both trailer fields.
        if not inflater.eof or len(inflater.unused_data) != 8 or inflater.unconsumed_tail:
            raise ValueError('truncated, trailing or oversized compressed stream')
        crc, size = struct.unpack('<II', inflater.unused_data)
        if size != expected or crc != zlib.crc32(data):
            raise ValueError('compressed checksum or size mismatch')
    else:
        data = raw
    if len(data) != expected:
        raise ValueError(f"{label}: inflated {len(data)} bytes, header says {expected}")
    return data, compressed, expected


def build_bundle(source_text: str, signature_size: int = 384) -> bytes:
    image, _, _ = _array_bytes(source_text, "IMAGE_PROD")
    header, _, _ = _array_bytes(source_text, "HEADER_PROD")
    signatures, _, _ = _array_bytes(source_text, "SIG_PROD")
    patch_loc, _, _ = _array_bytes(source_text, "PATCH_LOC")
    patch_meta, _, _ = _array_bytes(source_text, "PATCH_META")
    num_sigs, _, _ = _array_bytes(source_text, "NUM_SIGS")
    if len(header) != 36 or len(patch_loc) != 4 or len(patch_meta) != 12 or len(num_sigs) != 4:
        raise ValueError("unexpected BooterLoad record size")
    fields = struct.unpack("<9I", header)
    loc = struct.unpack("<I", patch_loc)[0]
    _fuse, engine, ucode = struct.unpack("<3I", patch_meta)
    count = struct.unpack("<I", num_sigs)[0]
    if not count or len(signatures) != count * signature_size or fields[4] != 1 or not ucode:
        raise ValueError('unsupported production Booter signature/application table')
    if loc < fields[2] or loc + signature_size > fields[2] + fields[3] or fields[2] + fields[3] > len(image):
        raise ValueError('signature patch is outside DMEM image')
    for offset, size in ((fields[0],fields[1]), (fields[2],fields[3]), (fields[5],fields[6])):
        if offset + size > len(image) or offset % 256 or size % 256:
            raise ValueError('invalid Falcon transfer range')
    hdr = struct.pack(
        "<14I",
        fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
        fields[7], fields[8], loc, ucode, engine, count, len(signatures),
    )
    prefix = struct.pack("<II", MAGIC, 1) + hdr + struct.pack("<II", len(image), len(signatures))
    return prefix + image + signatures


def extract_assets(source_root: Path, output: Path) -> None:
    inputs = {}
    for name in SOURCES:
        data = (source_root / 'src/nvidia/generated' / name).read_bytes()
        inputs[name] = data.decode('ascii')
    boot = inputs['g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c']
    image = _array_bytes(boot, 'UCODE_IMAGE_PROD')[0]
    desc = _array_bytes(boot, 'UCODE_DESC_PROD')[0]
    if len(desc) != 84:
        raise ValueError('unexpected bootloader descriptor length')
    fields = struct.unpack('<21I', desc)
    if fields[0] != 5 or fields[14] != 1 or fields[19] != len(image) or fields[20] != 0:
        raise ValueError('unsupported bootloader descriptor')
    for index in (1,3,8,10,12):
        if not fields[index+1] or fields[index]+fields[index+1] > len(image):
            raise ValueError('bootloader descriptor range outside image')
    blobs = {'gsp_boot_ga102.bin': image, 'gsp_boot_desc_ga102.bin': desc}
    for chip in ('GA100', 'GA102'):
        blobs[f'booter_load_{chip.lower()}.bin'] = build_bundle(
            inputs[f'g_bindata_kgspGetBinArchiveBooterLoadUcode_{chip}.c'])
    ga100_boot = inputs['g_bindata_kgspGetBinArchiveGspRmBoot_GA100.c']
    blobs['gsp_boot_ga100.bin'] = _array_bytes(ga100_boot, 'UCODE_IMAGE')[0]
    blobs['gsp_boot_desc_ga100.bin'] = _array_bytes(ga100_boot, 'UCODE_DESC')[0]
    loader = inputs['g_bindata_ksec2GetBinArchiveBlUcode_TU102.c']
    if not re.search(r'UCODE_DESC_data\s*=\s*\{\s*0xfd,\s*0,\s*\{\s*0x0,\s*0x200,\s*0x200,\s*0x100\s*}', loader):
        raise ValueError('unsupported generic loader descriptor')
    blobs['falcon_loader.bin'] = _array_bytes(loader, 'UCODE_IMAGE')[0]
    ad102_boot = inputs['g_bindata_kgspGetBinArchiveGspRmBoot_AD102.c']
    blobs['booter_load_ad102.bin'] = build_bundle(inputs['g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.c'])
    blobs['gsp_boot_ad102.bin'] = _array_bytes(ad102_boot, 'UCODE_IMAGE_PROD')[0]
    blobs['gsp_boot_desc_ad102.bin'] = _array_bytes(ad102_boot, 'UCODE_DESC_PROD')[0]
    tu102_boot = inputs['g_bindata_kgspGetBinArchiveGspRmBoot_TU102.c']
    tu102_image = _array_bytes(tu102_boot, 'UCODE_IMAGE')[0]
    tu102_desc = _array_bytes(tu102_boot, 'UCODE_DESC')[0]
    if len(tu102_desc) != 76 or struct.unpack('<I', tu102_desc[:4])[0] != 4 or \
            len(tu102_image) != 4096:
        raise ValueError('unsupported TU102 bootloader descriptor')
    tu102_bl = _array_bytes(
        inputs['g_bindata_ksec2GetBinArchiveBlUcode_TU102.c'], 'UCODE_IMAGE')[0]
    if len(tu102_bl) != 768:
        raise ValueError('unexpected TU102 SEC2 loader size')
    blobs['booter_load_tu102.bin'] = build_bundle(
        inputs['g_bindata_kgspGetBinArchiveBooterLoadUcode_TU102.c'], 16)
    blobs['gsp_boot_tu102.bin'] = tu102_image
    blobs['gsp_boot_desc_tu102.bin'] = tu102_desc
    blobs['sec2_bl_tu102.bin'] = tu102_bl
    output.mkdir(parents=True, exist_ok=True)
    for name, data in blobs.items():
        (output/name).write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help='NVIDIA source root')
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    extract_assets(args.source, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, zlib.error) as error:
        sys.exit(f"extract failed: {error}")
