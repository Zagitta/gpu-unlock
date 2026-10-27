# SPDX-License-Identifier: GPL-2.0-only
"""Run one transaction through the native runner and write evidence."""
from __future__ import annotations

import ctypes as C
import json
import os
import time
from pathlib import Path

from . import assets, build, codetable, inputs

MAX_UNKNOWN = 16


class Unknown(C.Structure):
    _fields_ = [
        ("write", C.c_bool),
        ("address", C.c_uint32),
        ("value", C.c_uint32),
        ("pc", C.c_uint32),
    ]


class Result(C.Structure):
    _fields_ = [
        ("driver_result", C.c_int),
        ("device_error", C.c_int),
        ("fwsec_launch_reached", C.c_bool),
        ("booter_launch_reached", C.c_bool),
        ("fwsec_executed", C.c_bool),
        ("fwsec_kdf_ok", C.c_bool),
        ("booter_executed", C.c_bool),
        ("booter_halted", C.c_bool),
        ("plm_opened", C.c_bool),
        ("target_written", C.c_bool),
        ("unlock_verified", C.c_bool),
        ("flr_seen", C.c_bool),
        ("launches", C.c_uint32 * 2),
        ("resets", C.c_uint32),
        ("host_writes", C.c_uint64),
        ("host_reads", C.c_uint64),
        ("engine_writes", C.c_uint64),
        ("engine_reads", C.c_uint64),
        ("instructions", C.c_uint64 * 2),
        ("unknown_count", C.c_uint32),
        ("unknown", Unknown * MAX_UNKNOWN),
        ("feats", C.c_uint32 * 8),
        ("feat_count", C.c_uint32),
        ("wpr2_lo", C.c_uint32),
        ("wpr2_hi", C.c_uint32),
        ("ss0", C.c_uint32),
        ("ss1", C.c_uint32),
        ("pci_command", C.c_uint32),
        ("dma_live", C.c_uint32),
        ("fault_pc", C.c_uint32),
        ("fault_opcode", C.c_uint32),
        ("plm_feat", C.c_uint32),
        ("plm_wpr_cfg", C.c_uint32),
        ("plm_wpr", C.c_uint32),
        ("denied_writes", C.c_uint32),
        ("booter_error", C.c_uint32),
        ("stage", C.c_uint32),
        ("fwsec_halt_pc", C.c_uint32),
        ("booter_halt_pc", C.c_uint32),
        ("fwsec_status", C.c_uint32),
        ("booter_status", C.c_uint32),
        ("booter_mailbox0", C.c_uint32),
        ("booter_mailbox1", C.c_uint32),
        ("booter_reset_plm", C.c_uint32),
        ("group03", C.c_uint32),
        ("fwsec_mac_ok", C.c_uint32),
        ("wpr2_normalized", C.c_uint32),
    ]


def load_runner(library: Path):
    lib = C.CDLL(str(library))
    lib.gd_run_transaction.argtypes = [
        C.c_uint16, C.c_uint32, C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p,
        C.c_char_p, C.c_char_p,
        C.c_int, C.c_int, C.POINTER(Result),
    ]
    lib.gd_run_transaction.restype = C.c_int
    return lib


def as_dict(family: str, manifest: dict, result: Result, elapsed: float) -> dict:
    return {
        "family": family,
        "run_dir": manifest.get("run_dir"),
        "device": manifest["device"],
        "chip": manifest["chip"],
        "fwsec_executed": bool(result.fwsec_executed),
        "fwsec_kdf_ok": bool(result.fwsec_kdf_ok),
        "booter_executed": bool(result.booter_executed),
        "booter_halted": bool(result.booter_halted),
        "plm_opened": bool(result.plm_opened),
        "target_written": bool(result.target_written),
        "unlock_verified": bool(result.unlock_verified),
        "driver_result": result.driver_result,
        "device_error": result.device_error,
        "fwsec_launch_reached": bool(result.fwsec_launch_reached),
        "booter_launch_reached": bool(result.booter_launch_reached),
        "flr_seen": bool(result.flr_seen),
        "launches": list(result.launches),
        "resets": result.resets,
        "host_writes": result.host_writes,
        "host_reads": result.host_reads,
        "engine_writes": result.engine_writes,
        "engine_reads": result.engine_reads,
        "instructions": list(result.instructions),
        "unknown": [
            {
                "write": bool(result.unknown[i].write),
                "address": result.unknown[i].address,
                "value": result.unknown[i].value,
                "pc": result.unknown[i].pc,
            }
            for i in range(min(result.unknown_count, MAX_UNKNOWN))
        ],
        "feats": list(result.feats)[: result.feat_count],
        "wpr2": [result.wpr2_lo, result.wpr2_hi],
        "selectors": [result.ss0, result.ss1],
        "pci_command": result.pci_command,
        "dma_live": result.dma_live,
        "fault_pc": result.fault_pc,
        "fault_opcode": result.fault_opcode,
        "plm_feat": result.plm_feat,
        "plm_wpr_cfg": result.plm_wpr_cfg,
        "plm_wpr": result.plm_wpr,
        "denied_writes": result.denied_writes,
        "booter_error": result.booter_error,
        "stage": STAGES.get(result.stage, result.stage),
        "fwsec_halt_pc": result.fwsec_halt_pc,
        "booter_halt_pc": result.booter_halt_pc,
        "fwsec_status": result.fwsec_status,
        "booter_status": result.booter_status,
        "booter_mailbox0": result.booter_mailbox0,
        "booter_mailbox1": result.booter_mailbox1,
        "booter_reset_plm": result.booter_reset_plm,
        "group03": result.group03,
        "fwsec_mac_ok": bool(result.fwsec_mac_ok),
        "wpr2_normalized": bool(result.wpr2_normalized),
        "elapsed_s": round(elapsed, 3),
    }


def run_transaction(family: str, run_dir: Path, *,
                    threads: bool = False, verbose: bool = False,
                    library: Path | None = None) -> dict:
    run_dir = Path(run_dir)
    manifest = inputs.prepare(family, run_dir)
    library = Path(library) if library else build.build()
    lib = load_runner(library)
    key_hex = os.environ.get("GPU_UNLOCK_DEBUG_KEY")
    key = bytes.fromhex(key_hex) if key_hex else None
    if key is not None and len(key) != 16:
        raise SystemExit("GPU_UNLOCK_DEBUG_KEY must be 16 bytes of hex")
    code_path = ""
    booter_code_path = ""
    if key is not None:
        codetable.build(library, run_dir / "rom.bin", key, run_dir / "code.bin")
        code_path = str(run_dir / "code.bin")
        booter = run_dir / "assets" / assets.FAMILIES[family]["booter"]
        codetable.build_booter(library, booter, key, run_dir / "booter_code.bin")
        booter_code_path = str(run_dir / "booter_code.bin")
    result = Result()
    started = time.monotonic()
    ret = lib.gd_run_transaction(
        manifest["device"], manifest["chip"],
        str(run_dir / "rom.bin").encode(),
        str(run_dir / "assets").encode(),
        str(run_dir / "trace.bin").encode(),
        code_path.encode(), booter_code_path.encode(), key,
        1 if threads else 0, 1 if verbose else 0, C.byref(result),
    )
    elapsed = time.monotonic() - started
    record = as_dict(family, manifest, result, elapsed)
    record["return"] = ret
    (run_dir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    decode_trace(run_dir / "trace.bin", run_dir / "trace.jsonl")
    return record


TRACE_KINDS = {0: "bar-read", 1: "bar-write", 2: "dma", 3: "start", 4: "halt", 5: "flr", 6: "fault", 7: "pc"}

STAGES = {
    0: "pre-fwsec",
    1: "fwsec-running",
    2: "fwsec-done",
    3: "wpr2-ready",
    4: "booter-running",
    5: "booter-halted",
    6: "booter-failed",
}


def decode_trace(binary: Path, text: Path):
    data = Path(binary).read_bytes()
    if len(data) < 4:
        return
    count = int.from_bytes(data[:4], "little")
    lines = []
    offset = 4
    for _ in range(count):
        if offset + 32 > len(data):
            break
        time_us, pc, address, value, kind, source, size = __import__("struct").unpack_from("<QIIIHHH", data, offset)
        offset += 32
        lines.append({"time_us": time_us, "pc": pc, "address": address,
                      "value": value, "kind": TRACE_KINDS.get(kind, kind),
                      "source": source, "size": size})
    Path(text).write_text("".join(json.dumps(line) + "\n" for line in lines))


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("family", choices=sorted(assets.FAMILIES))
    parser.add_argument("--run-dir", default=None)
    parser.add_argument("--threads", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    directory = Path(args.run_dir or (Path("build") / "runs" / args.family))
    print(json.dumps(run_transaction(
        args.family, directory, threads=args.threads, verbose=args.verbose), indent=2))
