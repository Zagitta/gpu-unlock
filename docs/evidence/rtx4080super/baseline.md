# Stock (locked) — RTX 4080 SUPER, 2026-10-23

Dense 8192² matmul medians, torch 2.11.0+cu128, seven samples after warmup.

| Mode | Time | Throughput |
| --- | ---: | ---: |
| FP32 (CUDA cores) | 29.30 ms | 37.53 TFLOPS |
| TF32 (tensor) | 19.68 ms | 55.86 TFLOPS |
| FP16 (torch mm, FP32 accumulate) | 9.94 ms | 110.62 TFLOPS |
| BF16 (torch mm, FP32 accumulate) | 9.94 ms | 110.65 TFLOPS |

Cold selectors (module entry): SS0=0x25121025, SS1=0x7, FEAT_OVR_PLM=0xffffff8f.
