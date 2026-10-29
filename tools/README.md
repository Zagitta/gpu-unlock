# Read-only SM issue-rate diagnostic

`read_sm_rates.py` queries NVIDIA RM command `0x20801230`. It does not change speed selectors: it creates temporary RM handles, reads the issue-rate fields, then closes the descriptors. It does not map BARs, write registers, upload firmware, reset the GPU or run CUDA kernels.

The getter and its original tests come from [JustAResearcher/gpu-unlock-ada at 4e7fefc](https://github.com/JustAResearcher/gpu-unlock-ada/tree/4e7fefcc4923f9e083d5324dc75a7d2f906af77f), under GPL-2.0-only. This version accepts a custom `nvidia-smi` executable path. The command and structure definitions were also checked against NVIDIA's [595.112.07 public header](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/d8257d95ae5fed364163e4df51db66df8b1ee0d2/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gr.h#L1531-L1635). That is a source check, not a hardware validation of this diagnostic.

Use an already working NVIDIA driver. The tool deliberately requires exactly one GPU and its exact UUID:

```sh
nvidia-smi --query-gpu=uuid --format=csv,noheader
python3 tools/read_sm_rates.py --expect-uuid GPU-REPLACE-WITH-EXACT-UUID
```

For a driver installed outside `PATH`, pass `--nvidia-smi /absolute/path/to/nvidia-smi`. Supply its existing library path through the environment if needed. If the device nodes require root, run the same command through `sudo -n env` with that environment explicitly supplied. Do not install another driver to run the diagnostic.

This version opens `/dev/nvidia0`. If hot-plug assigned the sole GPU a different minor number, that open can fail; nonzero-minor discovery is not supported.

The output reports RM's raw field values, not a measured speedup. A reduced issue-rate field does not establish an available override, the performance of a particular CUDA kernel, or an LLM throughput gain.

## Blackwell support

The upstream module's [admission table](https://github.com/Zagitta/gpu-unlock/blob/af28cbbfd3d922a659227ecfb7369adfd924cf03/kernel/gpu_unlock/main.c#L21-L39) covers GA102/GA103/GA104/GA106. This fork does not add GB206 or other Blackwell cards to it.

NVIDIA 595.112.07 [dispatches the newer bootstrap family separately](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/d8257d95ae5fed364163e4df51db66df8b1ee0d2/src/nvidia/generated/g_kernel_gsp_nvoc.c#L779-L805). Linux's [FSP documentation](https://www.kernel.org/doc/html/v7.3-rc7/gpu/nova/core/fsp.html) explains the difference: Hopper/Blackwell uses FSP to authenticate FMC, which loads GSP-RM, rather than the SEC2 Booter chain used by Ampere. A missing `BooterLoadUcode` HAL variant is therefore not evidence of a hidden Blackwell SEC2 booter waiting to be extracted.

Changing a PCI ID, CPU page size or installed driver version does not supply the missing Blackwell firmware path. No Blackwell speed change or performance gain has been measured by this fork.
