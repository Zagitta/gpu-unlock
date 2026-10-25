# RTX 4060 Laptop GPU (AD107M): experimental evidence

## Provenance and validation boundary

**The physical result below is from one experimental startup boot of a local
guarded fork, not from a physical boot of the proposed upstream module.** The
fork was based on
[`JustAResearcher/gpu-unlock-ada@491925c1079435e260137b9f3526066e764de830`](https://github.com/JustAResearcher/gpu-unlock-ada/tree/491925c1079435e260137b9f3526066e764de830),
with local AD107 board/chip guards. The startup transaction, subsequent NVIDIA
driver operation, RM rates and bounded compute measurements were observed. A
physical full-power-off action was instructed, but was not independently
observed or explicitly confirmed. No PM-resume experiment was performed.

The proposed implementation preserves upstream main's shared GSP ownership,
three-argument unlock API, card list and failure lifecycle. It builds on the
existing [Ada PR #24](https://github.com/Zagitta/gpu-unlock/pull/24) lineage at
`d8c57121f4976c861478be5d508750734d6587a6`, merged onto main
`af28cbbfd3d922a659227ecfb7369adfd924cf03`; it is not a replacement port copied
from the tested fork. Its host suite passed all five top-level Python tests,
including the real C selection/PM and transaction harnesses, with complete
pinned firmware assets. The final module metadata revision was also built
with GCC/Kbuild through BTF for `7.2.8-10-omarchy` and inspected with
`modinfo`. Those are **host-test and compilation evidence only**: the
proposed module has not been installed, loaded or physically startup-tested.

Files in this directory:

- [`boot-log.txt`](boot-log.txt): sanitized messages from the local-fork startup
  transaction. The header identifies the source; there is no root command line,
  hostname, boot ID, GPU UUID or VBIOS binary.
- [`measurements.json`](measurements.json): public board/software identity,
  transaction summary, named RM rates, **all five samples and all medians** for
  each benchmark mode, numerical checks and scope limitations.
- [`probe.py`](probe.py): self-contained reconstruction of the original Python
  CUDA Driver API/PTX measurement cells. It retains the original PTX and fixed
  workload, rather than substituting a GEMM benchmark. Reconstructing this
  source is not evidence that the proposed module ran on hardware.

## Exact board and opt-in

| Property | Observed value |
| --- | --- |
| GPU | NVIDIA GeForce RTX 4060 Laptop GPU, AD107M |
| PCI ID | `10de:28a0` |
| Subsystem | `1d05:1274`, AIstone Global Limited |
| Revision | `a1` |
| `PMC_BOOT_0` | `0x197000a1` |
| VBIOS version | `95.07.16.80.E1` (version only; no image distributed) |
| SMs / compute capability | 24 / 8.9 |
| Memory | 8 GiB physical; 8188 MiB reported usable |
| NVIDIA driver | Open kernel module `615.78.08` |
| Firmware inputs | Pinned `610.43.03`, unchanged Ada payload/assets |
| Stock benchmark kernel | `7.2.8-9-omarchy` |
| Unlocked startup/benchmark kernel | `7.2.8-10-omarchy` |

Only this PCI/subsystem/revision combination is initially admitted. There is
no sibling AD107 ID, desktop RTX 4060 or other laptop-board support claim.
Before layout register reads, coherent DMA allocations, BAR writes or firmware
start, the transaction additionally requires the exact `PMC_BOOT_0` above.

For a separately prepared startup experiment, the kernel command line must
contain **both** options, with the actual full PCI address of the board:

```text
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1
```

`ad107_experimental` is read-only after module load and defaults to off. AD107
is never selected through `targets=all`, a comma-separated list, a short BDF,
or opt-in without a target. Other supported cards retain their existing
selection behavior. Do not reload, reset or unbind a live GPU to try to reproduce
this pre-driver transaction.

Support is **startup-only / cold-boot-only**, meaning the cold, unbound
pre-driver code path, not proof that physical power removal was observed in the
historical run. AD107 is removed from the resume-retention list after startup;
the transaction API also rejects an AD107 `resume=true` call before acquiring
transaction resources. `gpu_unlock.resume=1` does not enable it. Resume behavior
of existing supported Ampere/Ada cards is unchanged.

## Transaction observations and inherited safety gates

The local-fork log records the common AD10x asset set:
`booter_load_ad102.bin`, `gsp_boot_ad102.bin`, `gsp_boot_desc_ad102.bin` and
`v67-ad102-610.43.03-four-plm.bin`, with shared `gsp_ga10x.bin`. The proposed
module uses that same pinned profile and every inherited Ada gate; it cannot
fall back to GA102 assets for the admitted AD107 device.

| Check | Local-fork observation |
| --- | --- |
| SEC2 mailbox success marker | `0x67b00001`, marker check passed |
| SEC2 completion | `CPUCTL=0x10`, halted before selector changes |
| WPR_CFG permission | `0x0004cb8f` to required `0xfffff0ff` |
| WPR permission | `0x0004cb8f` to required `0xffffffff` |
| FEAT permission | `0xffffff8f` to required `0xffffffff`, retained after FLR |
| FBPA permission | `0xffffff0f`, unchanged; inherited non-required write |
| SS0 / SS1 | `0x67267154` / `0x1` to `0x88888888` / `0x8` |
| Raw `FEATURE_READOUT_1` | `0x00410080` before selectors; `0x0` after selectors and FLR |
| PCIe FLR | Returned 0; post-FLR selectors and driver-entry containment checked |
| Driver handoff | NVIDIA bound and remained usable for the bounded measurement |

The log's printed readout-field labels are not independent validation of the
register's field layout. The evidence uses the **raw word**, named RM rates and
actual measured instructions, not the printed field decoder alone.

The board ROM's FWSEC image is `0x10300` bytes, with IMEM `0xf500` and DMEM
`0xd80`. Falcon DMA moves 256-byte blocks: the DMEM transfer covers `0xe00`
bytes and ends exactly at `0xf500 + 0xe00 = 0x10300`. PR24 already implements
rounded transfer-span validation, so this valid geometry does not require a
second port of [FWSEC PR #35](https://github.com/Zagitta/gpu-unlock/pull/35).
The host regression constructs a synthetic descriptor with these dimensions,
checks all 245 IMEM and 14 DMEM block submissions and the final DMA boundary,
and rejects a declaration extending beyond the image. No board firmware is
embedded in that test.

The new host coverage also rejects wrong board/PMC identity and direct resume
entry, checks consumer-visible AD10x payload/metadata, and exercises missing
required permissions, wrong marker, live SEC2, changed DMA payload, inaccessible
or nonzero raw readout, post-FLR loss, FWSEC/allocation failure and selector
readback failure. It preserves the upstream rule that firmware-exposed DMA
cannot be freed when safe containment is unproven.

The read-only RM command was `0x20801230`; its named rate codes were:

| Named rate | Stock | After the local-fork startup |
| --- | ---: | ---: |
| `imla0` | 0 | 0 |
| `fmla16` | 0 | 0 |
| `dp` | 0 | 0 |
| `fmla32` | 1 | 0 |
| `ffma` | 0 | 0 |
| `imla1` | 0 | 0 |
| `imla2` | 1 | 0 |
| `imla3` | 0 | 0 |
| `imla4` | 1 | 0 |

## Bounded instruction-throughput measurement

This is a register-resident Tensor Core instruction microbenchmark, **not dense
GEMM, LLM tokens/s or game FPS**. Identical PTX and dimensions were used in the
stock and local-fork unlocked states:

- PTX target `sm_89`; SHA-256
  `c937fdef3ed6719e46b05869c6983149d7c8ec8ad1bbcf8ef7ca33a13a097051`.
- 768 blocks, 256 threads/block, 1024 loop iterations, eight independent
  accumulator chains per warp.
- Five interleaved samples per mode, after three warm-up rounds and a
  16-iteration numerical preflight for every mode.
- Each numerical check reads all 196608 outputs. Every recorded check had zero
  mismatches, both at preflight and after each measured sample.
- Clocks were not locked and the desktop remained active. The NVIDIA version
  stayed the same, but the kernel version changed as shown above. NVML readings
  can be cached and are not precise measurements of instantaneous operating
  clocks, power or efficiency.

**Best of five**, not medians (floating-point modes in TF/s; INT8 in TOP/s):

| Mode | Stock best | Unlocked best | Best ratio |
| --- | ---: | ---: | ---: |
| FP16, FP16 accumulation | 59.385639 | 59.376880 | 1.000x |
| FP16, FP32 accumulation | 30.244118 | 57.050201 | 1.886x |
| BF16, FP32 accumulation | 30.230070 | 59.655639 | 1.973x |
| TF32, FP32 accumulation | 15.111773 | 29.826715 | 1.974x |
| INT8, INT32 accumulation | 119.459503 | 119.475010 | 1.000x |

For comparison, the **five-sample medians**, separately labeled:

| Mode | Stock median | Unlocked median |
| --- | ---: | ---: |
| FP16, FP16 accumulation | 59.369219 | 59.261634 |
| FP16, FP32 accumulation | 27.025290 | 54.386411 |
| BF16, FP32 accumulation | 26.919256 | 59.557473 |
| TF32, FP32 accumulation | 13.437898 | 29.814842 |
| INT8, INT32 accumulation | 119.438465 | 113.718738 |

Full-precision samples, times, telemetry and numerical expectations remain in
[`measurements.json`](measurements.json). The FP16-accumulation and INT8 controls
have essentially unchanged best rates, while FP32-accumulating FP16/BF16 and
TF32 increase. Unlocked clocks, desktop activity and the kernel change limit
causal precision; this is not a sustained-load or efficiency result.

## Reproducing the compute probe

The measured Python cells were made self-contained in [`probe.py`](probe.py).
The existing fork's cubin runner has a different grid/mode set and requires a
precompiled cubin; substituting it would not reproduce this exact workload.
The reconstruction retains the original PTX generator, mode order, operations
count, preflight, warm-up, interleaving and all-output validation. It requires
Python 3 and the installed driver's `libcuda.so.1` and `libnvidia-ml.so.1`, not a
CUDA toolkit or additional Python packages.

Without `--run`, this only generates PTX and reports its pinned hash; it does
not load the CUDA/NVML libraries or access a GPU:

```sh
python3 docs/evidence/rtx4060-laptop/probe.py
```

On an already established stock or unlocked startup state, explicitly select
the actual full PCI address:

```sh
python3 docs/evidence/rtx4060-laptop/probe.py --run --pci 0000:01:00.0
```

This requires exactly one CUDA-visible GPU, compute capability 8.9 and the
measured 24-SM configuration. It creates a CUDA context, runs bounded compute,
reads numerical results/NVML telemetry and releases its resources. It does not
query or print a GPU UUID, change clocks/power limits/selectors, manipulate
firmware or install/load/reset/unbind any module or device. GPU execution is an
explicit separate action.

During PR preparation, the standalone script's offline PTX hash matched the
historical hash above. Missing-PCI and Python `-O` execution were refused before
GPU execution. A separate `--run` smoke against the **already-running,
local-fork-unlocked GPU** completed in 0.760319 seconds: all 30 numerical checks
had zero mismatches and `cleanup_errors` was empty. One immediate bounded
kernel-log query found no matching entries. This validates the packaged compute
probe, **not a startup or firmware transaction of the proposed module**. The
historical stock/unlocked samples in `measurements.json` are unchanged; the
smoke is not a new before/after comparison or a sustained-stability result.

No Windows, suspend/resume, hibernation, second-board, multi-card, long-term,
application-throughput or proposed-module physical-validation claim is made.
A single read-only kernel-log query immediately after the historical bounded
probe found zero matching entries for `NVRM|Xid|nvidia|GPU|gpu_unlock` since the
recorded probe interval; that is not continuous monitoring or a general
stability guarantee.
