# Compatibility report: ASUS TUF Gaming F15 — RTX 3050 Mobile (10de:25a5)

## Result

**Unlocked**, with clear FP16/BF16/TF32 GEMM gains at similar SM clocks and power.

## System

| Field | Value |
|-------|--------|
| Laptop | ASUS TUF Gaming F15 class (Tiger Lake-H, HM570) |
| CPU host | Intel Tiger Lake-H (8086:9a26) |
| iGPU | Intel UHD (8086:9a68), hybrid graphics |
| GPU | NVIDIA GA107M GeForce RTX 3050 Mobile |
| PCI | `0000:01:00.0` `[10de:25a5]` (rev a1) |
| VRAM | 4 GiB GDDR6 |
| Distribution | Ubuntu 26.04.1 LTS (resolute) |
| Kernel | 7.0.0-38-generic |
| NVIDIA driver | 595.99.02 (proprietary) |
| Initramfs | **dracut** via `update-initramfs` (Ubuntu 26 default) |
| gpu-unlock | v1.1.0 + GA107 `0x25a5` recipe + dracut `force_drivers` drop-in |
| Install | `make install-deb` equivalent (DKMS + firmware + **install-dracut**) |

### Kernel command line

```text
gpu_unlock.targets=0000:01:00.0
```

## Boot verification (excerpt)

Successful unlock on cold boot:

```text
pci 0000:01:00.0: gpu_unlock: stage=host ss0=0x88888888 ss1=0x8 ...
pci 0000:01:00.0: gpu_unlock: stage=flr ... ss0=0x88888888 ss1=0x8 ...
pci 0000:01:00.0: gpu_unlock: unlocked
```

## Benchmarks

**Method:** PyTorch 2.13+cu132, `torch.mm`, dense GEMM **8192³**, FP32 accumulation for FP16/BF16 inputs, TF32 via `allow_tf32`. Median over 20 iterations × 5 runs unless noted.

### Sustained (~60 W default, opportunistic boost)

| Input | Median TFLOPS | SM clock / power (during run) |
|-------|---------------|-------------------------------|
| FP16 | 23.07 | ~1590 MHz, ~71 W |
| BF16 | 23.66 | ~1627 MHz, ~75 W |
| TF32 | 12.92 | ~1590 MHz, ~72 W |

### Peak (`nvidia-smi -pl 75`, SM locked 1800 MHz, 40 iters × 7 runs)

| Input | Peak | Median | Min |
|-------|------|--------|-----|
| FP16 | **25.69** | 22.98 | 20.02 |
| BF16 | **26.49** | 23.44 | 22.12 |
| TF32 | 12.76 | 10.96 | 10.03 |

Peak runs reached ~78–86 °C; TF32 peaked lower when SM clock dropped under thermals.

### Interpretation

These FP16/BF16 results match other **unlocked** GA107 laptop reports (low–mid 20s TFLOPS at ~1.5–1.6 GHz), not typical **half-rate** stock GEMM on the same class of GPU (~9–18 TFLOPS). Micro-benchmarks do not guarantee the same gain in every application.

## Install notes (Ubuntu 26)

1. On Ubuntu 26, `update-initramfs` invokes **dracut**. The package **must** install `packaging/boot/dracut.conf` (`make install-dracut` / `gpu-unlock-dkms` Debian package). Initramfs-tools drop-ins alone are insufficient on this distro.
2. After `make install` from a working tree with only `install-initramfs-tools`, verify initramfs contains `gpu_unlock.ko` (`lsinitramfs … | grep gpu_unlock`).
3. Optional: `packaging/boot/initramfs-tools-hook` helps distributions that build initramfs with classic **initramfs-tools** (calls `manual_add_modules gpu_unlock`).

## Suspend / resume

Not fully characterized on this machine. Hybrid Optimus laptops may reset selectors after RTD3/D3cold; see upstream README resume behaviour.
