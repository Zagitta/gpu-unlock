# Unlocked — RTX 4080 SUPER, 2026-10-23, same card and harness

Dense 8192² matmul medians, torch 2.11.0+cu128, seven samples after warmup.

| Mode | Time | Throughput | vs stock |
| --- | ---: | ---: | ---: |
| FP32 (CUDA cores) | 29.17 ms | 37.69 TFLOPS | 1.00x |
| TF32 (tensor) | 11.88 ms | 92.54 TFLOPS | 1.66x |
| FP16 (torch mm, FP32 accumulate) | 5.42 ms | 202.72 TFLOPS | 1.83x |
| BF16 (torch mm, FP32 accumulate) | 5.87 ms | 187.43 TFLOPS | 1.69x |

Sustained rates are power-limited (320 W board limit). The transaction
verified all four payload PLM writes, SS0=0x88888888 / SS1=0x8 readback,
a zero FEATURE_READOUT_1, and survived FLR into the stock driver.
