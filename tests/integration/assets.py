# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the ROM and asset directory for one family.

The committed VBIOS files are full containers.  The device exposes the same
1 MiB PROM window to the host driver and to the GSP secure bus; the driver
walks the IFR header to the embedded PCI/legacy image exactly as
kgspExtractVbiosFromRom_TU102 does.  Assets come from the existing gpu-unlock
cache (or GPU_UNLOCK_ASSETS) and are copied into the run directory; nothing is
fetched.
"""
from __future__ import annotations

import os
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VBIOS = ROOT / "emulator" / "vbios"
CACHE = Path(os.environ.get("GPU_UNLOCK_ASSETS", Path.home() / ".cache" / "gpu-unlock" / "assets"))
BOOTERS = Path(os.environ.get("GPU_UNLOCK_BOOTERS", Path.home() / "Projects" / "booters"))

PROM_SIZE = 0x100000
PROM_OFFSET = 0x300000

FAMILIES = {
    "GA100": {
        "device": 0x20C2,
        "chip": 0x170,
        "rom": VBIOS / "257744.rom",
        "offset": 0x0,
        "fwsec_version": 2,
        "booter": "booter_load_ga100.bin",
        "debug_booter": "ga100_secure.enc",
    },
    "GA10x": {
        "device": 0x2204,
        "chip": 0x172,
        "rom": VBIOS / "Palit.RTX3090.24576.210310_3.rom",
        "offset": 0x0,
        "fwsec_version": 3,
        "booter": "booter_load_ga102.bin",
        "debug_booter": "ga102_secure.enc",
    },
    "AD102": {
        "device": 0x2684,
        "chip": 0x192,
        "rom": VBIOS / "Palit.RTX4090.24576.221012.rom",
        "offset": 0x0,
        "fwsec_version": 3,
        "booter": "booter_load_ad102.bin",
        "debug_booter": "ad102_secure.enc",
    },
}

ASSETS = [
    "gsp_ga10x.bin", "gsp_boot_ga102.bin", "gsp_boot_desc_ga102.bin",
    "gsp_tu10x.bin", "gsp_boot_ga100.bin", "gsp_boot_desc_ga100.bin",
    "gsp_boot_ad102.bin", "gsp_boot_desc_ad102.bin",
    "falcon_loader.bin", "booter_load_ga100.bin", "booter_load_ga102.bin",
    "booter_load_ad102.bin",
]


def prepare_rom(family: str, output: Path) -> dict:
    spec = FAMILIES[family]
    data = spec["rom"].read_bytes()
    offset = spec["offset"]
    image = data[offset:offset + PROM_SIZE]
    if len(image) < 2 or not ((image[0] == 0x55 and image[1] == 0xAA) or
                              (image[0] == 0x4E and image[1] == 0x56)):
        raise SystemExit(f"{family}: no VBIOS image at {offset:#x} in {spec['rom']}")
    image = image.ljust(PROM_SIZE, b"\xff")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    return {"family": family, "rom": str(spec["rom"]), "offset": offset,
            "size": PROM_SIZE, "device": spec["device"], "chip": spec["chip"]}


def prepare_assets(family: str, destination: Path) -> list[str]:
    destination.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in ASSETS:
        source = CACHE / name
        if not source.exists():
            raise SystemExit(f"missing asset {source}; run firmware/fetch_artifacts.py first")
        shutil.copyfile(source, destination / name)
        copied.append(name)
    _install_debug_booter(family, destination / FAMILIES[family]["booter"])
    return copied


def _install_debug_booter(family: str, bundle: Path) -> None:
    """Install the external encrypted debug Booter IMEM.

    The driver loads one driver-format bundle and the HS loader decrypts the
    code.  The debug image is AES-ECB encrypted with the same debug key; the
    emulator performs that transform at the secure DMA/PIO boundary.  Header,
    DMEM data and signature area stay from the production bundle.
    """
    debug = BOOTERS / FAMILIES[family]["debug_booter"]
    if not debug.is_file():
        return
    data = bytearray(bundle.read_bytes())
    if len(data) < 80:
        raise SystemExit(f"{family}: short Booter bundle {bundle}")
    fields = struct.unpack_from("<14I", data, 8)
    code_off, code_size = fields[5], fields[6]
    start = 72 + code_off
    end = start + code_size
    code = debug.read_bytes()
    if len(code) != code_size:
        raise SystemExit(
            f"{family}: debug Booter {debug} is {len(code)} bytes; bundle needs {code_size}")
    if len(code) % 16:
        raise SystemExit(f"{family}: debug Booter {debug} is not AES block aligned")
    if end > len(data):
        raise SystemExit(f"{family}: Booter code range outside bundle")
    data[start:end] = code
    bundle.write_bytes(data)


def prepare_run(family: str, run_dir: Path) -> dict:
    run_dir.mkdir(parents=True, exist_ok=True)
    rom = run_dir / "rom.bin"
    manifest = prepare_rom(family, rom)
    assets = prepare_assets(family, run_dir / "assets")
    manifest["assets"] = assets
    manifest["run_dir"] = str(run_dir)
    return manifest
