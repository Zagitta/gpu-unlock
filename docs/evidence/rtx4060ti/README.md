# RTX 4060 Ti 8GB (AD106) evidence

One card (10de:2803), one boot, driver 615.71.09, firmware inputs
pinned to 610.43.03. The pinned source routes AD106 through the same
AD102 Booter/GspRmBoot archives as AD103, so the port is the device ID
plus routing the asset selection through is_ad103().

- `boot-log.txt` — successful transaction (`gpu_unlock: unlocked`),
  marker 0x67b00001, required permissions readback, readout1 zero
  after the selectors and after FLR.
- `benchmarks.md` — dense 8192² GEMM medians, stock vs unlocked,
  same card, same day, PyTorch 2.x/cu128.

| Mode | Stock | Unlocked |
| --- | ---: | ---: |
| FP16, FP32 accumulate | 46.76 TFLOPS | 70.68 TFLOPS (1.51x) |
| BF16, FP32 accumulate | 47.08 TFLOPS | 72.69 TFLOPS (1.54x) |
| TF32 | 22.99 TFLOPS | 33.41 TFLOPS (1.45x) |
| FP32 (CUDA cores) | 12.89 TFLOPS | 12.87 TFLOPS |

Sustained rates are power-limited (160 W board). Cold registers matched
the family: FEAT_OVR_PLM=0xffffff8f, WPR_CFG/WPR=0x4cb8f,
FBPA=0xffffff0f, readout1 fields 0 0 2 0 0 2 0 2 0, WPR2 down.
