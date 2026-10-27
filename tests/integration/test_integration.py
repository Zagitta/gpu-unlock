# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral gates for the native integration runner.

The keyed GA10x run executes the real FWSEC and SEC2 Booter and injects the
real V67 payload.  The payload does not yet reach a clean Booter halt, so the
final unlock checks fail honestly while the FWSEC and Booter execution evidence
is still asserted.
"""
from __future__ import annotations

import os
from contextlib import contextmanager
from pathlib import Path

from . import build, hooks
from .run import load_runner, run_transaction

ROOT = Path(__file__).resolve().parents[2]
RUNS = ROOT / "build" / "runs" / "tests"
FAILURES: list[str] = []
CHECKS = 0


def check(condition, message, detail=None, fail=None):
    global CHECKS

    CHECKS += 1
    if not condition:
        text = fail or f"not: {message}"
        FAILURES.append(text)
        print("FAIL:", text)
        if detail:
            print("      ", detail)
    else:
        print("ok:", message)


@contextmanager
def section(title):
    print(f"\n== {title} ==")
    checks = CHECKS
    failures = len(FAILURES)
    yield
    count = CHECKS - checks
    print(f"-- {title}: {count} check{'s' if count != 1 else ''}, "
          f"{len(FAILURES) - failures} failures")


def report(record, label):
    print(f"[{label}] driver={record['driver_result']} stage={record['stage']} "
          f"launches={record['launches']} unlocked={record['unlock_verified']}")
    print(f"    fwsec={record['fwsec_executed']} mac={record['fwsec_mac_ok']} "
          f"booter_insns={record['instructions'][0]} "
          f"halt={record['booter_halt_pc']:#x} fault={record['fault_pc']:#x} "
          f"status={record['booter_status']} reset_plm={record['booter_reset_plm']:#x}")


def main():
    print("gpu-unlock integration tests")
    print("driver diagnostics: off (set GPU_UNLOCK_DIAG=1 to show them)")
    RUNS.mkdir(parents=True, exist_ok=True)

    with section("build"):
        lib = build.build()
        print(f"library: {lib}")

    with section("unit self-tests"):
        runner = load_runner(lib)
        check(runner.gd_test_aes() == 1, "AES-128 FIPS-197 vector round-trips")
        check(runner.gd_test_wpr2() == 0, "WPR2 encoding and range vectors pass")
        check(hooks.check() == [], "TOML inventory and compiled hooks agree")

    key = os.environ.get("GPU_UNLOCK_DEBUG_KEY")
    if not key:
        check(False, "real path: GPU_UNLOCK_DEBUG_KEY is required")
        return 1

    with section("real path: FWSEC + Booter + V67 payload"):
        record = run_transaction("GA10x", RUNS / "GA10x", threads=True, library=lib)
        report(record, "GA10x real")
        check(record["fwsec_launch_reached"], "FWSEC launch observed")
        check(record["instructions"][1] > 1000, "real FWSEC executed")
        check(record["fwsec_executed"], "real FWSEC completed")
        check(record["fwsec_mac_ok"], "real FWSEC DMEM MAC matched")
        check(record["fwsec_kdf_ok"], "SCP op4 KDF produced the debug MAC")
        check(record["fwsec_status"] == 0, "real FWSEC left a clean status")
        check(record["booter_launch_reached"], "real Booter launch observed")
        check(record["instructions"][0] > 1000, "real Booter executed")
        check(record["booter_halt_pc"] != 0 or record["fault_pc"] != 0,
              "real Booter stop PC recorded")
        check(Path(record["run_dir"], "trace.jsonl").exists(), "trace decoded")
        # Final acceptance.  Report each failed precondition with the observed
        # state so the verdict is readable without decoding raw booleans.
        check(record["booter_halted"] and record["booter_status"] == 0 and
              (record["booter_reset_plm"] & 0xff) == 0xff,
              "real payload: Booter halted cleanly",
              f"halted={record['booter_halted']} status={record['booter_status']:#x} "
              f"reset_plm={record['booter_reset_plm']:#x} "
              f"halt_pc={record['booter_halt_pc']:#x} fault_pc={record['fault_pc']:#x}",
              fail="real payload: Booter did not halt cleanly")
        check(record["target_written"], "real payload: V67 wrote the target",
              f"target_written={record['target_written']} "
              f"plm_opened={record['plm_opened']}",
              fail="real payload: V67 did not write the target")
        check(record["unlock_verified"], "real payload: card reached the unlocked state",
              "requires a clean Booter halt, PLM open, target write, "
              f"selectors 0x88888888/0x8 and RESET_PLM=0xffffffff; observed "
              f"plm_opened={record['plm_opened']} "
              f"target_written={record['target_written']} "
              f"selectors={record['selectors'][0]:#010x}/{record['selectors'][1]:#x} "
              f"reset_plm={record['booter_reset_plm']:#x}",
              fail="real payload: card is not unlocked")

    print(f"\n{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
