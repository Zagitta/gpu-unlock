# SPDX-License-Identifier: GPL-2.0-only
"""Run-directory inputs: prepared ROM, assets and the per-run manifest."""
from __future__ import annotations

import json
from pathlib import Path

from . import assets


def prepare(family: str, run_dir: Path) -> dict:
    manifest = assets.prepare_run(family, run_dir)
    manifest["rom_sha256"] = __import__("hashlib").sha256(
        (Path(run_dir) / "rom.bin").read_bytes()).hexdigest()
    (Path(run_dir) / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest
