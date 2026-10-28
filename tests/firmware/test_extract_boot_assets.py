#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the generated-source parsing in extract_boot_assets.py.

Covers both NVIDIA bindata codegen styles: the byte-token arrays of
610.43.03 and the NvU64-word arrays of 615.71.10 reported in #168,
including the zero padding the NvU64 expansion adds beyond the declared
storage size.

Run: python3 tests/firmware/test_extract_boot_assets.py
"""
import importlib.util
import pathlib
import struct
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "extract_boot_assets", HERE.parent.parent / "firmware" / "extract_boot_assets.py")
extract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(extract)

LABEL = "TEST"


def u64_words(payload: bytes, extra_zero_words: int = 0) -> list[bytes]:
    words = [payload[i:i + 8].ljust(8, b"\0") for i in range(0, len(payload), 8)]
    words += [b"\0" * 8] * extra_zero_words
    return words


def tokens_bytes(payload: bytes) -> str:
    return ", ".join(f"0x{b:02x}" for b in payload)


def tokens_u64(words: list[bytes]) -> str:
    return ", ".join(f"0x{int.from_bytes(word, 'little'):016x}ULL" for word in words)


def generated_source(data: bytes, style: str, extra_zero_words: int = 0) -> str:
    """Uncompressed BINDATA label rendered in one of the two codegen styles."""
    if style == "bytes":
        aligned, tokens = "NV_DECLARE_ALIGNED(NvU8, 8)", tokens_bytes(data)
    elif style == "u64":
        aligned, tokens = "NV_DECLARE_ALIGNED(NvU64, 8)", tokens_u64(u64_words(data, extra_zero_words))
    else:
        raise AssertionError(style)
    return (
        "// COMPRESSION: NO\n"
        f"// DATA SIZE (bytes): {len(data)}\n"
        f"static BINDATA_CONST {aligned} archive_BINDATA_LABEL_{LABEL}_data[] = \n"
        "{\n    " + tokens + ",\n};\n"
    )


def generated_compressed_source(data: bytes, style: str, extra_zero_words: int = 0) -> str:
    """Raw-DEFLATE stream plus the gzip CRC32/ISIZE trailer, as NVIDIA BINDATA stores it."""
    stream = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = stream.compress(data) + stream.flush()
    payload += struct.pack("<II", zlib.crc32(data), len(data))
    if style == "bytes":
        aligned, tokens = "NV_DECLARE_ALIGNED(NvU8, 8)", tokens_bytes(payload)
        stored = len(payload)
    elif style == "u64":
        words = u64_words(payload, extra_zero_words)
        aligned, tokens = "NV_DECLARE_ALIGNED(NvU64, 8)", tokens_u64(words)
        # The declared size stays exact; the array itself is padded up to a
        # whole number of 8-byte words (see #168).
        stored = len(payload)
    else:
        raise AssertionError(style)
    return (
        "// COMPRESSION: YES\n"
        f"// DATA SIZE (bytes): {len(data)}\n"
        f"// COMPRESSED SIZE (bytes): {stored}\n"
        f"static BINDATA_CONST {aligned} archive_BINDATA_LABEL_{LABEL}_data[] = \n"
        "{\n    " + tokens + ",\n};\n"
    )


def expect_error(text: str, word: str) -> None:
    try:
        extract._array_bytes(text, LABEL)
    except ValueError as error:
        assert word in str(error), (word, str(error))
        return
    raise AssertionError("expected a ValueError containing " + word)


def main() -> int:
    data = bytes(range(256)) * 3 + b"gpu-unlock"  # 778 bytes: not a multiple of 8

    # 610.43.03 style, uncompressed and compressed (unchanged behaviour).
    assert extract._array_bytes(generated_source(data, "bytes"), LABEL)[0] == data
    assert extract._array_bytes(generated_compressed_source(data, "bytes"), LABEL)[0] == data

    # 615.71.10 style: the same stream as NvU64 words.  The word alignment
    # pads the array past the declared size; the padding is zero.
    assert extract._array_bytes(generated_source(data, "u64"), LABEL)[0] == data
    assert extract._array_bytes(generated_compressed_source(data, "u64"), LABEL)[0] == data
    assert extract._array_bytes(generated_source(data, "u64", extra_zero_words=2), LABEL)[0] == data
    assert extract._array_bytes(generated_compressed_source(data, "u64", extra_zero_words=1),
                                LABEL)[0] == data

    # Padding must be all zero; anything else is a corrupt asset.  Poison the
    # final word of the array, which lies entirely in the padding region.
    words = u64_words(data)
    words[-1] = words[-1][:-1] + b"\x01"
    poisoned = (
        "// COMPRESSION: NO\n"
        f"// DATA SIZE (bytes): {len(data)}\n"
        f"static BINDATA_CONST NV_DECLARE_ALIGNED(NvU64, 8) archive_BINDATA_LABEL_{LABEL}_data[] = \n"
        "{\n    " + tokens_u64(words) + ",\n};\n"
    )
    expect_error(poisoned, "declared size")
    compressed = generated_compressed_source(data, "u64", extra_zero_words=1)
    expect_error(compressed.replace("0x0000000000000000ULL,\n};",
                                    "0x0100000000000000ULL,\n};"),
                 "storage size mismatch")

    # Mixed or oversized tokens stay rejected.
    expect_error(generated_source(data, "bytes").replace("0x00,", "0x1122,"), "non-byte token")
    expect_error(generated_source(data, "u64").replace("ULL,", "ULL, 0x" + "f" * 17 + "ULL", 1),
                 "non-byte token")

    # A byte-style array shorter than the declared size stays rejected.
    truncated = generated_compressed_source(data, "bytes").rsplit(",", 40)[0] + "};\n"
    expect_error(truncated, "storage size mismatch")

    print("all extract_boot_assets tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
