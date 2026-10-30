#!/usr/bin/env python3
"""Benchmark the GEMM throughput affected by the Ampere GPU unlock.

Run this once before enabling gpu_unlock.targets and once after rebooting with
the unlock enabled. Keep the GPU power limit and all arguments identical.
"""

from __future__ import annotations

import argparse
import json
import statistics
import time

import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=8192,
                        help="square matrix dimension (default: 8192)")
    parser.add_argument("--warmup", type=int, default=3,
                        help="unmeasured GEMMs per data type (default: 3)")
    parser.add_argument("--samples", type=int, default=5,
                        help="measured GEMMs per data type (default: 5)")
    parser.add_argument("--label", default="",
                        help="run label, for example 'before' or 'after'")
    parser.add_argument("--json", action="store_true",
                        help="emit JSON instead of a Markdown table")
    return parser.parse_args()


def set_tf32(enabled: bool) -> None:
    # This remains the most widely supported switch across PyTorch releases.
    torch.backends.cuda.matmul.allow_tf32 = enabled


@torch.inference_mode()
def measure(size: int, dtype: torch.dtype, tf32: bool,
            warmup: int, samples: int) -> tuple[float, list[float]]:
    set_tf32(tf32)
    left = torch.randn((size, size), device="cuda", dtype=dtype)
    right = torch.randn((size, size), device="cuda", dtype=dtype)

    for _ in range(warmup):
        torch.mm(left, right)
    torch.cuda.synchronize()

    rates: list[float] = []
    operations = 2.0 * size**3
    for _ in range(samples):
        start = time.perf_counter()
        torch.mm(left, right)
        torch.cuda.synchronize()
        elapsed = time.perf_counter() - start
        rates.append(operations / elapsed / 1e12)
    return statistics.median(rates), rates


def main() -> int:
    args = parse_args()
    if not torch.cuda.is_available():
        raise SystemExit("CUDA is unavailable; this benchmark requires an NVIDIA GPU")
    if args.size <= 0 or args.warmup < 0 or args.samples <= 0:
        raise SystemExit("--size and --samples must be positive; --warmup cannot be negative")

    device = torch.cuda.get_device_properties(torch.cuda.current_device())
    cases = (
        ("FP16", torch.float16, False),
        ("BF16", torch.bfloat16, False),
        ("TF32", torch.float32, True),
    )
    results: dict[str, object] = {
        "label": args.label,
        "gpu": device.name,
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "matrix_size": args.size,
        "warmup": args.warmup,
        "samples": args.samples,
        "results": {},
    }

    for name, dtype, tf32 in cases:
        median, rates = measure(args.size, dtype, tf32, args.warmup, args.samples)
        results["results"][name] = {  # type: ignore[index]
            "median_tflops": round(median, 3),
            "samples_tflops": [round(value, 3) for value in rates],
        }

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        label = f" ({args.label})" if args.label else ""
        print(f"{device.name}{label}; torch {torch.__version__}, CUDA {torch.version.cuda}")
        print(f"{args.size}^3 dense GEMM; {args.samples}-sample median")
        print("| Input | Median TFLOPS | Samples (TFLOPS) |")
        print("| ----- | ------------- | ----------------- |")
        for name, _, _ in cases:
            item = results["results"][name]  # type: ignore[index]
            values = ", ".join(f"{value:.3f}" for value in item["samples_tflops"])
            print(f"| {name} | {item['median_tflops']:.3f} | {values} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
