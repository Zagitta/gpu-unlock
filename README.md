# GPU unlock 1.0.0

Unlock FP/BF16 instruction issue rate on Nvidia 3000's cards to their professional equivalents

This Linux kernel module combines the CMP unlocker's V67/PLM technique with the pre-driver timing of the UEFI approach. It runs the unlock, writes the speed selectors (SS0 `0x88888888`, which selects the full BF16 rate, and the 4-bit SS1 `0x8` IMLA4) and performs FLR before the unmodified NVIDIA driver binds the card. It is neither a UEFI application nor a patched NVIDIA driver. 

The benefit of this early kmod driver is that it does not require manual patching each time nvidia releases a new driver version.

The effect of this unlock is that it doubles the instruction issuing speed of FP/BF16 FMA into FP32 operations on GA102, GA103, GA104, GA106 and GA107 cards.

[3090 Ti dense 8192³ GEMM at 450 W, five-sample medians with FP32 accumulation](https://github.com/Zagitta/gpu-unlock/issues/1#issue-5905622687):


| Input | Before       | After         |
| ----- | ------------ | ------------- |
| FP16  | 87.46 TFLOPS | 118.39 TFLOPS |
| BF16  | 87.50 TFLOPS | 157.41 TFLOPS |
| TF32  | 42.58 TFLOPS | 82.61 TFLOPS  |

## Tested cards

Results reported by users. The module accepts every GeForce ID listed in
`kernel/gpu_unlock/main.c`, but the cards below are the ones someone has
reported on. Please add yours with a
[compatibility report](https://github.com/Zagitta/gpu-unlock/issues/new?template=compatibility-report.yml),
whether it worked or not.

| Chip  | Card                              | PCI ID      | Result                              | BF16 GEMM gain | Reports |
| ----- | --------------------------------- | ----------- | ----------------------------------- | -------------- | ------- |
| GA102 | RTX 3090 Ti                       | `10de:2203` | Unlocked                            | +68–80%        | [#1](https://github.com/Zagitta/gpu-unlock/issues/1) |
| GA102 | RTX 3090                          | `10de:2204` | Unlocked                            | +25–67%        | [#5](https://github.com/Zagitta/gpu-unlock/issues/5), [#9](https://github.com/Zagitta/gpu-unlock/issues/9), [#10](https://github.com/Zagitta/gpu-unlock/issues/10), [#17](https://github.com/Zagitta/gpu-unlock/issues/17), [#25](https://github.com/Zagitta/gpu-unlock/issues/25), [#27](https://github.com/Zagitta/gpu-unlock/issues/27), [#33](https://github.com/Zagitta/gpu-unlock/issues/33) |
| GA102 | RTX 3080 Ti 20 GB (OEM, GA102-250) | `10de:2205` | Unlock completes, BF16 stays at half rate | —        | [#28](https://github.com/Zagitta/gpu-unlock/issues/28) |
| GA102 | RTX 3080 10 GB                    | `10de:2206` | Unlocked                            | +70–73%        | [#14](https://github.com/Zagitta/gpu-unlock/issues/14) |
| GA102 | RTX 3080 20 GB (memory upgrade)   | `10de:2206` | Unlocked                            | +56–59%        | [#10](https://github.com/Zagitta/gpu-unlock/issues/10), [#15](https://github.com/Zagitta/gpu-unlock/issues/15) |
| GA104 | RTX 3070                          | `10de:2484` | Unlocked                            | +73–81%        | [#10](https://github.com/Zagitta/gpu-unlock/issues/10), [#12](https://github.com/Zagitta/gpu-unlock/issues/12) |
| GA104 | RTX 3070 Ti                       | `10de:2482` | Not tested with this module (the same unlock worked from a UEFI port on Windows) | — | [#32](https://github.com/Zagitta/gpu-unlock/issues/32) |
| GA106 | RTX 3060 12 GB                    | `10de:2504` | Unlocked                            | about +80%     | [#13](https://github.com/Zagitta/gpu-unlock/pull/13), [#31](https://github.com/Zagitta/gpu-unlock/issues/31) |
| GA103 | any                               | `10de:2414` | No reports yet                      | —              | — |
| GA107 | RTX 3050 6 GB Laptop (mobile)      | `10de:25ec` | Unlocked via resume only — cold-probe unlock does not survive `nvidia` binding on this board, the existing resume re-unlock does | ~100% (register MMA; see [case study](docs/Extending%20to%20a%20new%20laptop%20GPU.md)) | this PR |

The gains are dense GEMM throughput from each report's own benchmark, so
they are not directly comparable. The power limit matters a lot: the lowest
figure (+25%) is from cards capped at 225 W.

Matrix-heavy work speeds up: LLM prefill (+14–36% in
[#9](https://github.com/Zagitta/gpu-unlock/issues/9),
[#15](https://github.com/Zagitta/gpu-unlock/issues/15),
[#25](https://github.com/Zagitta/gpu-unlock/issues/25) and
[#33](https://github.com/Zagitta/gpu-unlock/issues/33)), diffusion and
FlashAttention. LLM decode, which is limited by memory bandwidth, and INT8
work stay about the same. The reports that compared outputs found them
bit-identical before and after.

Reports also cover up to four cards in one machine, the card driving the
display, and cards passed through to a VM with vfio-pci
([#17](https://github.com/Zagitta/gpu-unlock/issues/17)).




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



## Local build

Every build downloads the pinned NVIDIA/CachyOS inputs over HTTPS, checks
their SHA-256, and generates firmware only in the build directory and the
binary package. All three formats install through the same `make install-*`
targets. Packages are built from the committed `HEAD`.

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
gpu_unlock.targets=all                          # every unbound GA102/GA103/GA104/GA106 card
gpu_unlock.targets=0000:21:00.0                 # only these cards
gpu_unlock.targets=0000:21:00.0,0000:22:00.0    # comma-separated
```

Addresses are full PCI names as `lspci -D` prints them (hex digits in either
case), independent of GPU numbering and display role. An entry that is not
a GA102/GA103/GA104/GA106 card, including `all` mixed with addresses, is logged and ignored.
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
lspci -nnk -s 0000:21:00.0
```

To stop unlocking, remove `gpu_unlock.targets=` from the kernel command line;
for a one-off boot, delete it in the bootloader's edit screen. Removing the
packages also removes the module and the initramfs drop-ins.

If the firmware on a card never stops, the module logs it and keeps the DMA
buffers that firmware may still use until the next power off. Power off
before trying again.

The kernel module and local packaging are GPL-2.0-only. The generated payload
derives from the pinned CMP reference; NVIDIA firmware remains under its own
terms. See `firmware/NOTICE.md` and `licenses/` for attribution and notices.

# Support me

Help me buy more GPUs <3

[https://buymeacoffee.com/zagitta](https://buymeacoffee.com/zagitta)  

ETH: 0xf4eee937937f92038D70B16AbD7736D8a53a1c5c

BTC: bc1qwpkwp70fn76z3s4kg2l9y3j4kst95twcznuugf

SOL: 8Rih6HY7V4LcdjYHz4iSffT4fTniqw5CACaBigSVxNYu