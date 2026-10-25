# RTX 3080 Laptop 16 GB (GA104M, 10de:24dc): stock versus unlocked

**Result: unlocked, with measured speed gains on the physical GPU.** Tested on 2026-10-25 using the unmodified kernel-module sources at commit `af28cbbfd3d922a659227ecfb7369adfd924cf03` (gpu-unlock 1.0.0). The device ID was already in the supported list; this report does not add support code.

The main comparison covers **62 timed configurations**: 59 GEMM/attention/quantized-linear/LLM/VLM cases and three full-fine-tuning sequence lengths. One additional 512-token training repeat checks observed run-to-run variation. QLoRA was not rerun.

## Hardware and software

| Item | Configuration |
|---|---|
| Laptop | Lenovo Legion 7-16ACHg6, product 82N6 |
| CPU | AMD Ryzen 9 5900HX |
| GPU | NVIDIA RTX 3080 Laptop GPU, GA104M, 16 GB VRAM |
| PCI ID / subsystem / revision | `10de:24dc` / `17aa:3a58` / A1 |
| PCI address | `0000:01:00.0` |
| VBIOS | `94.04.3F.00.C5`; no VBIOS flash as part of this experiment |
| GPU role | Display-driving GPU, bare metal |
| Power configuration | Existing 150 W cTGP plus up to 15 W Dynamic Boost; approximately 165 W observed under load |
| OS / kernel | Ubuntu 22.04.5 LTS / `6.8.0-138-generic` |
| NVIDIA driver | `580.178.04`, proprietary kernel module |
| Secure Boot | Enabled; locally signed DKMS module accepted after MOK enrollment |
| Initramfs | initramfs-tools |
| Python / PyTorch / CUDA build | 3.10.18 / 2.8.0 / CUDA 12.8 |
| Transformers / bitsandbytes / Accelerate | 4.57.6 / 0.48.1 / 1.12.0 |
| Runtime | Eager PyTorch/Hugging Face; no torch.compile or serving engine |

All comparisons used the same driver, kernel, packages, model files and test inputs. AC power remained connected and the desktop remained active. The existing power configuration was retained; the unlock did not raise the configured power limit.

## Installation and boot evidence

Built the repository's native Debian packages with `make deb` and the pinned NVIDIA 610.43.03 firmware inputs, then installed those packages. Building used GCC 12.3 and Python 3.11; the benchmark environment remained Python 3.10. The NVIDIA runtime driver remained 580.178.04.

Local installation details, without kernel-module source changes:

- Restricted this module's DKMS build to `6.8.0-138-generic`: the machine has older installed headers and a build against 5.15.0-198 failed because `linux/aperture.h` is missing. No kernel upgrade was needed.
- Enrolled the existing local MOK and verified the signed module loads with Secure Boot enabled.
- The initramfs contains the unlock module and all five firmware assets. It contains no NVIDIA graphics modules, so the normal upstream initramfs-tools integration loaded gpu_unlock before NVIDIA; the early-hook workaround from issue #11 was not needed.
- Enabled the normal Ubuntu GRUB configuration with `gpu_unlock.targets=all gpu_unlock.resume=0` and performed a full shutdown/start. Other relevant existing boot options were `iommu=off amd_iommu=off`.
- Resume reapplication was deliberately disabled for this first comparison. Suspend/resume and hibernation were **not tested**.

Relevant boot-log excerpt (hostnames removed):

```text
[    2.586837] gpu_unlock: loading out-of-tree module taints kernel.
[    3.883684] pci 0000:01:00.0: gpu_unlock: unlocked
[    5.708430] NVRM: loading NVIDIA UNIX x86_64 Kernel Module  580.178.04  Tue Jul  7 12:38:21 UTC 2026
```

This is a different laptop/PCI variant from the RTX 3080 Laptop report in [#81](https://github.com/Zagitta/gpu-unlock/issues/81), which used `10de:249c`. This report specifically validates `10de:24dc` on the above system.

## Benchmark method

- Same computational benchmark implementations and settings before/after. Base seed 20261025 (GEMM uses base seed + matrix size; attention uses base seed + sequence length), eight PyTorch CPU threads, and `CUBLAS_WORKSPACE_CONFIG=:4096:8`. Three warmups and five CUDA-event timing samples for GEMM, with repeated calls per sample targeting approximately 0.35 seconds. Model prefill uses two warmups; generation uses a separate warmup and five fixed-length measurements.
- Dense GEMM sizes: 2048, 4096 and 8192. FP16/BF16 reduced-precision reductions disabled; FP16 accumulation enabled only for the explicitly labeled FP16-accumulation control. TF32 enabled only for its row. INT8 uses PyTorch's integer matrix multiply with INT32 output. Correctness includes finite outputs, sampled FP64 references using the actual rounded inputs, and whole-output hashes.
- FlashAttention uses forced PyTorch Flash SDPA, causal attention, batch 1, 16 heads, head dimension 128, sequence lengths 2048/4096/8192; both forward and forward+backward, FP16 and BF16.
- Quantized linear: bitsandbytes NF4, double-quantized weights and BF16 compute; 4096 × 4096 weights and 1/128/2048 input tokens, including runtime dequantization.
- Qwen3-4B: BF16 and bitsandbytes NF4/BF16, fully GPU-resident. Fixed unpadded prompts at batch/length pairs (1,512), (1,2048), (1,8192), (4,512), (4,2048). Explicit unpadded causal Flash SDPA with native GQA. Prefill does not reuse a prefix cache and computes only final-position logits. Generation measures 128 forced greedy tokens per request with a KV cache; TTFT includes the initial cached prefill, and decode excludes that first token. The custom generation loop is checked against Transformers generate.
- Qwen3-VL-8B-Instruct: NF4/BF16, fixed synthetic single-image, OCR-document and two-image cases; separate fresh prefill, TTFT and 128-token decode. Image preprocessing, tokenization, model loading and weight quantization are excluded from timings.
- Full fine-tuning: pretrained Qwen3-0.6B, all 596,049,920 parameters trainable, no adapters/weight quantization/CPU offload. FP32 master weights, gradients and AdamW states; BF16 autocast, gradient checkpointing, microbatch 1, accumulation 1, AdamW learning rate 2e-5 with `foreach=False`, no gradient clipping. Five warmup and 50 measured optimizer steps per sequence, each loaded from the original checkpoint in a fresh process.
- Full-training data: first 128 [Stanford Alpaca](https://github.com/tatsu-lab/stanford_alpaca) examples, packed deterministically, loss on all tokens. Dataset SHA-256 `4d6c22bf9e4d92b5d8fede1a861b84fe830e9358a519c199736dd248a9fbe25c`; CC BY-NC 4.0, used here as a research throughput benchmark.
- GPU telemetry sampled every 0.5 seconds. GPU-event and synchronized wall times are retained separately; no HTTP/tokenizer/model-loading overhead in inference timings.

Pinned model revisions, with model-file SHA-256 verification before loading:

| Model | Revision |
|---|---|
| Qwen/Qwen3-4B | `1cfa9a7208912126459214e8b04321603b3df60c` |
| Qwen/Qwen3-VL-8B-Instruct | `0c351dd01ed87e9c1b53cbc748cba10e6187ff3b` |
| Qwen/Qwen3-0.6B | `c1899de289a04d12100db370d81485cdf75e47ca` |

Speedup means stock time / unlocked time, or unlocked throughput / stock throughput. For example, 1.65× is 65% higher throughput, not a 65% latency reduction.

## Large matrix multiplication (8192 × 8192 × 8192)

| Mode | Stock | Unlocked | Speedup | Throughput gain |
|---|---:|---:|---:|---:|
| bf16_fp32acc | 41.23 | 68.04 | 1.650× | +65.0% |
| fp16_fp32acc | 41.08 | 69.04 | 1.681× | +68.1% |
| tf32 | 20.62 | 34.82 | 1.689× | +68.9% |
| fp16_fp16acc | 63.36 | 63.30 | 0.999× | -0.1% |
| fp32_simt | 13.81 | 13.81 | 1.000× | -0.0% |
| int8_int32acc | 133.25 | 133.38 | 1.001× | +0.1% |

For the BF16 row, sampled mean power was 143.9 → 162.9 W and mean SM clock was 1797 → 1521 MHz; memory clock remained 7001 MHz. Greater throughput with lower SM clock and power near the existing ceiling is consistent with the unlock increasing math throughput while power limits constrain the realized gain.

Units are TFLOPS except INT8, which uses TOPS. FP16/BF16 accumulation modes are explicitly controlled by the original runner.

## FlashAttention

| Format | Sequence | Operation | Stock ms | Unlocked ms | Speedup |
|---|---:|---|---:|---:|---:|
| torch.float16 | 2048 | forward | 0.546 | 0.370 | 1.475× |
| torch.float16 | 2048 | forward_backward | 2.102 | 1.478 | 1.421× |
| torch.float16 | 4096 | forward | 1.978 | 1.357 | 1.458× |
| torch.float16 | 4096 | forward_backward | 7.256 | 5.038 | 1.440× |
| torch.float16 | 8192 | forward | 7.719 | 5.361 | 1.440× |
| torch.float16 | 8192 | forward_backward | 27.161 | 18.894 | 1.438× |
| torch.bfloat16 | 2048 | forward | 0.546 | 0.363 | 1.506× |
| torch.bfloat16 | 2048 | forward_backward | 2.106 | 1.465 | 1.438× |
| torch.bfloat16 | 4096 | forward | 1.967 | 1.331 | 1.478× |
| torch.bfloat16 | 4096 | forward_backward | 7.271 | 4.957 | 1.467× |
| torch.bfloat16 | 8192 | forward | 7.616 | 5.255 | 1.449× |
| torch.bfloat16 | 8192 | forward_backward | 27.086 | 18.439 | 1.469× |

## NF4 linear layer

Bitsandbytes NF4 weights with BF16 compute, including runtime dequantization. These results apply to this implementation and workload.

| Input tokens | Stock ms | Unlocked ms | Speedup |
|---:|---:|---:|---:|
| 1 | 0.100 | 0.093 | 1.076× |
| 128 | 0.304 | 0.257 | 1.181× |
| 2048 | 2.011 | 1.197 | 1.680× |

## LLM prefill — Qwen3-4B

Fresh forwards without prefix-cache reuse; aggregate input tokens/s. B=4 processes four prompts simultaneously.

| Format | Batch | Tokens/request | Stock tok/s | Unlocked tok/s | Speedup |
|---|---:|---:|---:|---:|---:|
| BF16 | 1 | 512 | 3415.5 | 4978.9 | 1.458× |
| BF16 | 1 | 2048 | 3700.9 | 5629.9 | 1.521× |
| BF16 | 1 | 8192 | 3217.3 | 4886.6 | 1.519× |
| BF16 | 4 | 512 | 3876.1 | 5843.8 | 1.508× |
| BF16 | 4 | 2048 | 3835.7 | 5754.3 | 1.500× |
| NF4_BF16_compute | 1 | 512 | 2927.7 | 3964.8 | 1.354× |
| NF4_BF16_compute | 1 | 2048 | 3522.5 | 5249.8 | 1.490× |
| NF4_BF16_compute | 1 | 8192 | 3175.1 | 4807.0 | 1.514× |
| NF4_BF16_compute | 4 | 512 | 3686.5 | 5437.5 | 1.475× |
| NF4_BF16_compute | 4 | 2048 | 3766.3 | 5656.1 | 1.502× |

## LLM generation — Qwen3-4B

128 forced output tokens/request, same eager Transformers loop. Decode excludes first-token latency. Batch throughput is aggregate.

| Format | Batch | Prompt tokens/request | Stock TTFT ms | Unlocked TTFT ms | Stock decode tok/s | Unlocked decode tok/s | Decode speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| BF16 | 1 | 512 | 148.2 | 103.0 | 27.54 | 27.34 | 0.993× |
| BF16 | 1 | 2048 | 553.7 | 365.6 | 27.53 | 27.81 | 1.010× |
| BF16 | 1 | 8192 | 2558.6 | 1691.5 | 27.50 | 27.82 | 1.012× |
| BF16 | 4 | 512 | 530.9 | 350.8 | 110.62 | 110.80 | 1.002× |
| BF16 | 4 | 2048 | 2160.2 | 1435.1 | 101.09 | 107.08 | 1.059× |
| NF4_BF16_compute | 1 | 512 | 177.3 | 129.1 | 20.38 | 20.65 | 1.013× |
| NF4_BF16_compute | 1 | 2048 | 581.9 | 390.7 | 20.00 | 20.50 | 1.025× |
| NF4_BF16_compute | 1 | 8192 | 2580.6 | 1708.4 | 20.45 | 20.64 | 1.009× |
| NF4_BF16_compute | 4 | 512 | 561.4 | 379.4 | 53.86 | 57.60 | 1.070× |
| NF4_BF16_compute | 4 | 2048 | 2191.1 | 1465.5 | 55.87 | 57.36 | 1.027× |

## VLM — Qwen3-VL-8B NF4

Prefill includes vision encoding and language prefill. Fixed synthetic image/OCR/two-image inputs; no image preprocessing or model loading in the timings.

| Case | Stock prefill ms | Unlocked prefill ms | Prefill speedup | Stock decode tok/s | Unlocked decode tok/s |
|---|---:|---:|---:|---:|---:|
| single_image | 240.05 | 183.07 | 1.311× | 19.13 | 19.24 |
| document_ocr | 407.87 | 285.87 | 1.427× | 19.06 | 19.20 |
| multiple_images | 498.69 | 346.75 | 1.438× | 18.99 | 19.29 |

## Full fine-tuning — Qwen3-0.6B

All 596,049,920 parameters are trainable. FP32 master weights/gradients/AdamW states, BF16 autocast, gradient checkpointing, microbatch 1; five warmups plus 50 measured optimizer steps per sequence. Same pretrained checkpoint and deterministic Alpaca packing.

| Sequence | Stock s/step | Unlocked s/step | Step speedup | Stock tok/s | Unlocked tok/s | Peak allocated GiB |
|---:|---:|---:|---:|---:|---:|---:|
| 512 | 0.3293 | 0.3049 | 1.080× | 1527.2 | 1653.0 | 10.11 |
| 1024 | 0.4893 | 0.4207 | 1.163× | 2073.6 | 2415.0 | 10.11 |
| 2048 | 0.7877 | 0.6466 | 1.218× | 2592.5 | 3161.5 | 10.98 |

### Training phase times

| Sequence | Phase | Stock ms | Unlocked ms | Speedup |
|---:|---|---:|---:|---:|
| 512 | Forward + loss | 65.16 | 62.33 | 1.045× |
| 512 | Backward | 138.38 | 117.55 | 1.177× |
| 512 | AdamW | 125.91 | 124.66 | 1.010× |
| 1024 | Forward + loss | 99.00 | 79.66 | 1.243× |
| 1024 | Backward | 264.56 | 215.09 | 1.230× |
| 1024 | AdamW | 125.34 | 124.64 | 1.006× |
| 2048 | Forward + loss | 174.47 | 137.36 | 1.270× |
| 2048 | Backward | 487.30 | 383.51 | 1.271× |
| 2048 | AdamW | 125.49 | 124.66 | 1.007× |

Phase medians use CUDA events; step medians use synchronized wall time. Phase medians need not sum exactly to the full-step median. Tokens/s uses total tokens divided by total measured wall time.

## Correctness and GPU health

- Completed 62 configurations with 0 unresolved failures; all saved finite-value checks passed.
- 107/107 corresponding output/logit/sample hashes match the stock run. This covers the saved GEMM/attention/linear outputs, prefill logits, generation samples, and instruction responses; it excludes training trajectories.
- All 5 manual-generation checks matched Transformers generate.
- Peak sampled GPU temperature: 76 °C. Peak sampled GPU power: 165.18 W.
- No NVIDIA Xid or GPU-fallen-off-bus errors were found in the current boot. No suspend occurred during the comparison.
- All full-fine-tuning runs had finite gradients, updated sampled base weights, and trained every parameter. No fine-tuned checkpoint was saved.

- Full fine-tuning sequence 512: maximum corresponding measured-loss difference 0.0185476542.
- Full fine-tuning sequence 1024: maximum corresponding measured-loss difference 0.032356441.
- Full fine-tuning sequence 2048: maximum corresponding measured-loss difference 0.0255710296.

**Training trajectories were not bit-identical.** All three initial forward losses matched stock exactly, but losses diverged after optimizer updates. Finite gradients and continued training do not establish identical convergence or model quality.

### Training repeatability control

One additional 512-token run repeated the same 50 measured full-training steps with the GPU still unlocked. All saved configuration/environment fields except the start time matched. Maximum corresponding loss difference between the two unlocked runs was **0.010881**, compared with **0.018548** between stock and the first unlocked run at that length.

Throughput was 1653.0 and 1663.6 tokens/s in the two unlocked runs. This demonstrates run-to-run variation in this training setup; it does not isolate or prove the cause of every stock/unlocked loss difference. A deterministic gradient/convergence study would be needed for that. The extra repeat is separate from the 62 primary matched timing configurations.

## Controls and interpretation limits

- MOK enrollment also allowed the existing Lenovo control module/service to load. Before activation, a separate stock repeat reran all 30 matrix/attention configurations. Every large matrix mode stayed within 0.6% of the original baseline; all 24 stored microbenchmark output hashes matched. Model comparisons use the original stock runs, so unmeasured system-state differences cannot be ruled out.
- The original micro/quantized-linear/VLM stages recorded earlier whole-file runner hashes while later model-only setup fixes were added. Their measured computational implementations were unchanged. The full-training runner and configuration hashes matched exactly before/after.
- One active-unlock session, with five inference/kernel samples per case and 50 measured full-training steps per sequence. These are not confidence intervals across independent rebooted trials.
- Exact output agreement applies to the stored inference/kernel outputs. Training losses were not bit-identical; the same-state repeat demonstrated variation too, without isolating the cause of every stock/unlocked difference. This is not a convergence/model-quality validation.
- VLM cases are functional fixtures rather than a comprehensive accuracy benchmark. Bitsandbytes NF4 results do not automatically generalize to GGUF, AWQ, GPTQ or Marlin.
- The full-training model is Qwen3-0.6B. These results do not establish whether full fine-tuning benefits more than QLoRA on the same model; QLoRA was excluded from the unlocked comparison.
- The runtime unlock is volatile. All measurements were made without suspend, with resume reapplication disabled. No claim is made about suspend/resume or other kernels/drivers/power configurations.

## Numeric data

The accompanying [matched-metrics CSV](rtx-3080-laptop-24dc.csv) contains the measured before/after values and speedup for all 62 matched configurations, including the smaller GEMM sizes. It is a summary of the saved measurements, not a reusable benchmark harness or a dump of raw per-iteration traces. No model weights, dataset records, generated firmware or machine-specific installation scripts are included.
