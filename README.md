![gpu-unlock: full-speed tensor cores for GeForce RTX](assets/banner.svg)

### Your GeForce runs AI at half speed. This fixes it.

![Version](https://img.shields.io/badge/version-2.0.0-76b900)![RTX 20 | 30 | 40](https://img.shields.io/badge/GeForce-RTX%2020%20%7C%2030%20%7C%2040-76b900?logo=nvidia&logoColor=white)![Linux](https://img.shields.io/badge/platform-Linux-informational?logo=linux&logoColor=white)![License GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue)![Stars](https://img.shields.io/github/stars/Zagitta/gpu-unlock?style=flat&color=yellow)

**Up to 2× FP16 / BF16 / TF32 tensor throughput** · **+25–55% LLM prefill** · **+25–50% fine-tuning** · **+30–55% image generation**

[Supported GPUs](#supported-gpus) · [Benchmarks](#what-you-get) · [Install](#how-to-install) · [FAQ](#faq) · [Support](#support-me)

---

NVIDIA ships GeForce cards with their tensor cores capped to **half speed** for
FP16, BF16 and TF32 math with FP32 accumulation, which is what nearly all AI
training and unquantized inference uses. The same silicon sold as an RTX A-series
or professional card runs at full rate. `gpu_unlock` removes that cap at boot,
before the stock NVIDIA driver loads. No patched driver, no VBIOS flash, nothing
to redo when NVIDIA releases a new driver.

---

## Supported GPUs

| Generation                | Cards                                                                                                                  | Status                    |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------- | ------------------------- |
| **Ampere** (RTX 30)       | RTX 3050, 3060, 3060 Ti, 3070, 3070 Ti, 3080, 3080 Ti, 3090, 3090 Ti, plus RTX 3070/3080 Laptop GPUs                   | Stable, 100+ user reports |
| **Ada Lovelace** (RTX 40) | RTX 4060, 4060 Ti, 4070, 4070 SUPER, 4070 Ti, 4070 Ti SUPER, 4080, 4080 SUPER, 4090, 4090 D, RTX 4050/4060 Laptop GPUs | Experimental              |
| **Turing** (RTX 20)       | RTX 2060, 2060 SUPER, 2070, 2070 SUPER, 2080, 2080 SUPER, 2080 Ti, Titan RTX                                           | Experimental              |
| Blackwell (RTX 50)        |                                                                                                                        | Not supported             |

Laptops only stay unlocked if the dedicated GPU is never fully powered off.  
Many laptops power it down when idle, which silently resets the unlock. Turning  
off runtime power management with `options nvidia NVreg_DynamicPowerManagement=0x00`
in `/etc/modprobe.d/` helps on some models, but expect mixed results.

The exact PCI device IDs are in [`kernel/gpu_unlock/recipes.c`](kernel/gpu_unlock/recipes.c).
Linux only. For Windows see the [FAQ](#faq).

---

## What you get

The tensor core rate doubles. How much of that you see depends on how much of
your workload is FP16/BF16/TF32 matrix math, and on whether the card has the
power headroom to run it.

Dense matrix multiply (FP32 accumulation, stock power limits):

| Card | BF16 before | BF16 after | Gain |
| --- | --- | --- | --- |
| RTX 3060 12 GB | 27 TFLOPS | 51 TFLOPS | +87% |
| RTX 3060 Ti | 37 TFLOPS | 68 TFLOPS | +82% |
| RTX 3070 | 40 TFLOPS | 72 TFLOPS | +81% |
| RTX 3080 10 GB | 65 TFLOPS | 113 TFLOPS | +73% |
| RTX 3090 | 75 TFLOPS | 125 TFLOPS | +65% |
| RTX 3090 Ti (450 W) | 88 TFLOPS | 157 TFLOPS | +80% |
| RTX 4070 Ti | 81 TFLOPS | 150 TFLOPS | +84% |
| RTX 4080 | 104 TFLOPS | 193 TFLOPS | +87% |
| RTX 4090 | 165 TFLOPS | 329 TFLOPS | +100% (peak MMA rate) |

The RTX 4090 runs out of power before it runs out of tensor cores, so large
GEMMs at stock power limits gain much less than the peak rate suggests.

Real workloads:

| Workload | Typical speedup |
| --- | --- |
| LLM prefill / prompt processing, FP16/BF16 or AWQ/GPTQ (vLLM, PyTorch, Transformers) | +25–55% |
| Time to first token | 25–30% lower |
| LoRA and full fine-tuning in BF16 | +25–50% |
| Image generation, FP16/BF16 (SDXL, SD 1.5, Krea, DiT models in ComfyUI) | +30–55% |
| Video generation, BF16 compute (Wan 2.2) | +15–20% |
| FlashAttention / SDPA kernels | +50–60% |
| llama.cpp / Ollama with quantized GGUF | 0–15%, mostly at long context |
| LLM decode / token generation (memory bound) | No change |
| INT8, FP8 and INT4 models | No change |
| Gaming and FP32 CUDA | No change |

Outputs are bit-identical before and after; only the speed changes. These
numbers come from the [compatibility reports](https://github.com/Zagitta/gpu-unlock/issues)
users have filed, where you can find full benchmark details and post your own.

---

## How to install

First run:

```sh
git clone https://github.com/Zagitta/gpu-unlock
cd gpu-unlock
make install
```

Then append `gpu_unlock.targets=all` to your kernel command line, keeping
existing options:

- **GRUB (Debian/Ubuntu/Mint):** add it inside `GRUB_CMDLINE_LINUX_DEFAULT`
in `/etc/default/grub`, then run `sudo update-grub`.
- **GRUB (Arch/CachyOS):** same edit, then run
`sudo grub-mkconfig -o /boot/grub/grub.cfg`.
- **Fedora/RHEL:** run
`sudo grubby --update-kernel=ALL --args="gpu_unlock.targets=all"`.
- **systemd-boot:** append it to the `options` line in your boot entry
(`bootctl status` shows its location).
- **UKI / generated entries:** add it to `/etc/kernel/cmdline` and regenerate
your boot images/entries (e.g. `sudo mkinitcpio -P` for mkinitcpio UKIs).

# FAQ

- Q: Do you have a website/coin/installer?  
A: No this is the one and only OG source for gpu-unlock. Crypto, websites, installers and what else is not associated with this project or the developer in any way shape or form. 
- Q: Does this depend on a specific driver version? I see `610.43.03` is pinned.  
A: No the driver is only fetched to extract firmware this unlock depends on    
- Q: What about Windows?  
A: I don't run windows and don't plan to maintain it but [https://github.com/impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK) has built a port, YMMV.   
There are windows installer's floating around but I would recommend being ++**very**++ careful with those.
- Q: What about Blackwell (RTX 50)?  
A: Not supported and unlikely to happen.    
- Q: Why did Nvidia handicap my GPU?  
A: Market segmentation, they want businesses to purchase the professional cards without these limitations at significantly higher prices.
- Q: Does this hurt my GPU?  
A: It's very unlikely, this only unlocks a small handicap Nvidia has put in place to differentiate datacenter and professional cards from consumer versions. It may heat up your GPU slightly because it now does more compute but the standard throttling and power management remains completely intact and should downclock.    
- Q: How can I assist?  
A: Please consider [donating](#support-me) so I can purchase more GPUs <3

## Local build

Every build downloads the NVIDIA/CachyOS inputs over HTTPS and generates
firmware only in the build directory and the binary package. All three formats install through the same `make install-*`
targets. Packages are built from the working tree, uncommitted changes included.

Build, install build dependencies, and install the packages in one step
(asks for your sudo password):

```sh
make install        # detect the distribution and select an installer
make install-arch   # makepkg -sif
make install-deb    # apt-get build-dep, dpkg-buildpackage, apt-get install
make install-rpm    # dnf builddep, rpmbuild, dnf install
```

`make install` uses `/etc/os-release` (`ID`, then `ID_LIKE`) to detect Arch,
Debian/Ubuntu, or Fedora/RHEL/CentOS families. The RPM installer requires DNF.
For other distributions, select a compatible explicit target above.

`make arch`, `make deb` and `make rpm` only build. Debian and RPM packages go
to `build/deb` and `build/rpm`. The NVIDIA inputs are cached in
`~/.cache/gpu-unlock/inputs` (`make inputs`; override with `INPUTS=`).
`dpkg-buildpackage` itself never uses the network, so a clean-chroot build (sbuild or
pbuilder) needs `GPU_UNLOCK_INPUTS` pointing at that cache.
The `make dist` archive, the Debian source package and the `.nosrc.rpm`
contain no NVIDIA archives or generated firmware.

## How it works

This Linux kernel module combines the cmpunlocker's V67/PLM technique with the
pre-driver timing of the UEFI approach. At boot it runs the unlock, writes the
speed selectors (SS0 `0x88888888`, which selects the full BF16 rate, and the
4-bit SS1 `0x8` IMLA4) and performs an FLR before the unmodified NVIDIA driver
binds the card. It is neither a UEFI application nor a patched NVIDIA driver.

## Boot flow and opt-in

The packages put `gpu_unlock` and its firmware in the initramfs and load it
there, before `nvidia` can bind a card:

- **mkinitcpio:** `/etc/mkinitcpio.conf.d/zz-gpu-unlock.conf` inserts
`gpu_unlock` right before `nvidia` in `MODULES`, so a display card is blank
for as short a time as possible. Without `nvidia` in the image, it goes
last and still runs before an `nvidia` loaded from the root filesystem.
Images are rebuilt on install. mkinitcpio ignores drop-ins for an image
built with `-c <config>` or a preset `_config`; put `gpu_unlock` right
before `nvidia` in that config's `MODULES` yourself.
- **initramfs-tools:** `/usr/share/initramfs-tools/modules.d/gpu-unlock.conf`.
- **dracut:** `/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf`.

With initramfs-tools and dracut, the module is built for every installed
kernel that has headers, and those kernels' images are rebuilt on install
and again on removal. Kernels installed later get the module and a new
image from the kernel's own install hooks.

There is no `softdep nvidia pre:` rule: kmod honours only the first
`softdep nvidia` line it reads, so other packages' rules (`nvidia-utils`,
`gddr6_hwmon`) would silently override it, or it would override theirs.

The module does nothing unless the kernel command line opts in:

```
gpu_unlock.targets=all                          # every supported unbound card
gpu_unlock.targets=0000:21:00.0                 # only these cards
gpu_unlock.targets=0000:21:00.0,0000:22:00.0    # comma-separated
```

Addresses are full PCI names as `lspci -D` prints them (hex digits in either
case), independent of GPU numbering and display role. An entry that is not
a supported card (see [Supported GPUs](#supported-gpus)), including `all` mixed with addresses, is logged and ignored.
The cards are unlocked concurrently, about a second in all however many
there are, and each on its own: a card that is already bound or powered
down is skipped. Bus mastering that the board firmware left on (Supermicro
X11 does this whatever its option ROM and bus master settings) is cleared
first; a card some driver already initialised is still refused. Then
`nvidia` loads and binds every card as usual.

The display card works too. Its firmware framebuffer (the boot console and
Plymouth) keeps running while the unlock's firmware steps run, and is
removed just before the card's reset, as `nvidia-drm` would do a moment
later anyway. Its screen is blank from that reset until `nvidia-drm` takes
over.

Loading the module by hand after boot does nothing to a card that `nvidia`
has bound.

Suspend to RAM powers the cards off, which resets the speed selectors. After
resume the module unlocks every card it unlocked at boot again, before
`nvidia` resumes it: its suspend notifier runs just ahead of the one `nvidia`
resumes its cards from (`NVreg_UseKernelSuspendNotifiers=1`, the default in
current drivers; `nvidia-resume.service` runs later still). This adds about a
second to resume. A card that kept power, as in suspend to idle, is still
unlocked and is only read. A card that `nvidia` has already resumed, or that is
bound to another driver, is left as it is, and the log says why. Add
`gpu_unlock.resume=0` to the kernel command line, or write `0` to
`/sys/module/gpu_unlock/parameters/resume`, to turn this off. Hibernation is
not covered.

Check the result with:

```sh
journalctl -k -b | grep gpu_unlock
lspci -nnk -s 0000:[ID]:00.0
```

To stop unlocking, remove `gpu_unlock.targets=` from the kernel command line;
for a one-off boot, delete it in the bootloader's edit screen. Removing the
packages also removes the module and the initramfs drop-ins.

If the firmware on a card never stops, the module logs it and keeps the DMA
buffers that firmware may still use until the next power off. Power off
before trying again.

The kernel module and local packaging are GPL-2.0-only. The generated buffer
derives from the pinned CMP reference; NVIDIA firmware remains under its own
terms. See `firmware/NOTICE.md` and `licenses/` for attribution and notices.

# Support me

Help me buy more GPUs <3

[Github Sponsor](https://github.com/sponsors/Zagitta)

[https://buymeacoffee.com/zagitta](https://buymeacoffee.com/zagitta) 

ETH: 0xf4eee937937f92038D70B16AbD7736D8a53a1c5c

BTC: bc1qwpkwp70fn76z3s4kg2l9y3j4kst95twcznuugf

SOL: 8Rih6HY7V4LcdjYHz4iSffT4fTniqw5CACaBigSVxNYu