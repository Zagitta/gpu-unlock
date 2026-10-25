# HP RTX 3080 Ti Laptop 16GB (GA103): Windows UEFI result

Reported by [Komeiji-Shiki](https://github.com/Komeiji-Shiki) in [#88](https://github.com/Zagitta/gpu-unlock/issues/88).

Successfully unlocked an **HP RTX 3080 Ti Laptop 16GB (GA103, `10de:2460`)** using a Windows UEFI adaptation of [rough2515-hub/rtx3080-unlock-public](https://github.com/rough2515-hub/rtx3080-unlock-public).

The laptop uses hybrid graphics, with the Intel iGPU driving the display. After the UEFI transaction and FLR, Windows read back:

```text
FEAT_PLM = 0xffffffff
SS0      = 0x88888888
SS1      = 0x00000008
```

## Performance

8192 × 8192 dense GEMM at a reported 175W power cap:

| Input | Before | After | Gain |
| --- | ---: | ---: | ---: |
| FP16 | 53.27 TFLOPS | 83.31 TFLOPS | +56.39% |
| BF16 | 53.66 TFLOPS | 84.57 TFLOPS | +57.60% |

Both runs used the same `torch.matmul` script: 10 warmups and 40 CUDA-event samples, taking the middle sorted sample. The script left PyTorch's precision settings unchanged. A separate successful USB run reached 85.09 TFLOPS FP16 and 85.01 TFLOPS BF16.

## What needed changing

- **Enable PCI memory access first.** The dGPU entered UEFI in D0 with PCI Command set to zero. Enabling memory decoding through `PciIo->Attributes()` made MMIO accessible. The original PCI state was restored after cleanup.
- **Handle both observed initial selector pairs.** Initial captures showed `SS0=0x02042060 / SS1=5`, while a Windows restart produced `0/0`. The exact-value precheck had to accept both before the restart path worked.
- **Use the complete PROM for FWSEC.** The firmware's ROM copy was truncated. Reading the full PROM supplied FWSEC and the extended ROM images. The existing layout calculation worked with 16GiB.
- **The GA102-named 610.43.03 firmware assets worked on GA103.** The FWSEC/Booter/V67 sequence completed and the selectors survived FLR.
- **Keep the GPU awake on AC.** We observed the selectors return to their original values during Windows troubleshooting. Later runs retained the unlock with a CUDA-context hold and a runtime power-management workaround, while still reaching P8 at about 10W idle.

The final setup boots the UEFI application from the internal ESP and chainloads Windows Boot Manager automatically.

**Tested: Windows UEFI port. Linux kernel module: not tested.**

Thanks to Zagitta/gpu-unlock for the unlock technique and rough2515-hub/rtx3080-unlock-public for the UEFI port.
