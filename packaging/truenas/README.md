# gpu-unlock on TrueNAS SCALE

Runs `gpu_unlock` on TrueNAS SCALE without dkms, apt, initramfs or GRUB
changes, and without touching `/usr`, the NVIDIA driver or the VBIOS. The
module is built off the box against the host's own kernel headers and
loaded by a small script just before TrueNAS's middleware loads `nvidia`.

Tested on TrueNAS SCALE 25.10.7 (kernel `6.12.105-production+truenas`,
NVIDIA 580.173.02) with an RTX 3060 12 GB LHR (`10de:2504`).

| File | Runs on | Purpose |
| --- | --- | --- |
| `host-diagnostics.sh` | TrueNAS, as root | Read-only report plus a tarball of the kernel headers |
| `build.sh` | Any Linux box with Docker | Firmware + module build in `debian:bookworm`, checks, bundle |
| `gpu-unlock-load.sh` | TrueNAS, from the bundle | Loads the module for one card; never fails the boot |

## Why it is done this way

- `nvidia.ko` is not on the TrueNAS root filesystem. It is in the
  `nvidia.raw` system extension, which middleware links into
  `/run/extensions` and merges, then runs
  `modprobe -a nvidia nvidia_drm nvidia_modeset`
  (`docker.configure_nvidia`, called from the docker plugin's `setup()`)
  while **`middlewared.service` starts**.
- Pool import (`ix-zfs`) runs after middlewared, and TrueNAS init scripts
  (`ix-preinit`, `ix-postinit`) after that, so an init script is always too
  late: once `nvidia` has booted the card's GSP, `gpu_unlock` refuses it
  until a full power-off.
- So the loader runs as an `ExecStartPre` drop-in on `middlewared.service`,
  from the boot pool. `/etc` and `/root` are writable datasets of the boot
  environment (`/` and `/usr` are read-only; `/data` and `/home` are
  `noexec`).
- TrueNAS has no supported hook this early; the drop-in is the one
  unsupported change. Both files belong to the current boot environment
  only, so an update boots stock until the module is rebuilt.
- The kernel is built with module signing but does not enforce it
  (`sig_enforce=N`), so the unsigned module loads; the NVIDIA module already
  taints the kernel the same way.
- The upstream Debian packages are not used: they need dkms,
  `update-initramfs` and a GRUB edit on a read-only root, and none of that
  survives an update.

## Build

1. On TrueNAS, collect the headers and a report (read-only apart from the
   output directory):
   ```sh
   sudo bash host-diagnostics.sh            # writes /tmp/gpu-unlock-diag
   ```
   Check the report: `sig_enforce` must be `N`, `docker nvidia: True`, and
   your card's `lspci -vvv` must show `FLReset+`.
2. Copy `/tmp/gpu-unlock-diag` to a Linux machine with Docker and Python 3,
   and from the repository root run:
   ```sh
   packaging/truenas/build.sh <copied gpu-unlock-diag dir>
   ```
   It downloads the pinned NVIDIA inputs once, generates the firmware,
   checks that the container's GCC and pahole match the kernel's, builds with
   `-Werror`, checks vermagic and every imported symbol's CRC against
   `Module.symvers`, and writes
   `packaging/truenas/out/gpu-unlock-<KVER>.tar.gz`.
   The firmware is NVIDIA's; the bundle is for your own machine only.

## Install

Below, `<KVER>` is the kernel version (`uname -r`) and `<GPU>` the card's
address as `lspci -D` prints it, e.g. `0000:2b:00.0`. Nothing takes effect
until the next full power-off.

1. Copy the bundle to `/tmp` on TrueNAS, then:
   ```sh
   sudo mkdir -p /root/gpu-unlock
   sudo chmod 700 /root/gpu-unlock
   sudo tar --no-same-owner -xzf /tmp/gpu-unlock-<KVER>.tar.gz -C /root/gpu-unlock
   sudo chown -R root:root /root/gpu-unlock
   sudo chmod -R go-w /root/gpu-unlock
   sudo sh -c 'cd /root/gpu-unlock/gpu-unlock-<KVER> && sha256sum --quiet -c SHA256SUMS && echo BUNDLE OK'
   ```
2. Add the drop-in:
   ```sh
   sudo mkdir -p /etc/systemd/system/middlewared.service.d
   printf '[Service]\nEnvironment=GPU=%s\nExecStartPre=-%s\n' \
     '<GPU>' /root/gpu-unlock/gpu-unlock-<KVER>/gpu-unlock-load.sh |
     sudo tee /etc/systemd/system/middlewared.service.d/gpu-unlock.conf
   sudo systemctl daemon-reload
   ```
   Do not restart middlewared: the card is already in use, so the loader
   would only skip it, and every TrueNAS service restarts with it.
3. Power off fully, then power on. A warm reboot is not enough after
   `nvidia` has used the card. A card that is the boot display stays blank
   for a moment while it resets.
4. Check:
   ```sh
   journalctl -b -t gpu-unlock             # loader: insmod exit 0
   journalctl -k -b | grep gpu_unlock      # "gpu_unlock: unlocked"
   lspci -nnk -s <GPU> | grep 'in use'     # Kernel driver in use: nvidia
   nvidia-smi
   ```

The loader adds about a second to boot. It does nothing, and middlewared
starts as usual, when `gpu_unlock.disable` is on the kernel command line,
`GPU` is unset, the running kernel is not the one the module was built for,
the card is missing or already bound, or the bundle checksum fails.
If an unlock fails part-way, the card can stay unusable until a full
power-off; pools, network and middleware are unaffected.

## Rollback

| Situation | Action |
| --- | --- |
| Stop using it | `sudo rm /etc/systemd/system/middlewared.service.d/gpu-unlock.conf && sudo systemctl daemon-reload`, then a full power-off. A warm reboot may keep the card unlocked. |
| Skip it for one boot | At GRUB press `e`, append `gpu_unlock.disable` to the `linux` line, Ctrl-X. |
| Anything worse | Boot the previous boot environment from GRUB; it has neither file. |
| Remove completely | Also `sudo rm -r /root/gpu-unlock`. |

## After a TrueNAS update

The new boot environment has neither the drop-in nor the bundle, and a new
kernel needs a new module, so the card runs stock. Re-run
`host-diagnostics.sh`, check that middleware still loads `nvidia` the same
way, rebuild, and repeat the install with the new `<KVER>`.

## Expectations

The unlock only speeds up FP16/BF16/TF32 tensor math with FP32
accumulation. ggml-based backends (llama.cpp, stable-diffusion.cpp,
trellis2.cpp and similar in LocalAI) often use FP16 accumulation or INT8,
which already run at full rate. On the test machine (RTX 3060, LocalAI),
diffusion, flow and audio workloads gained 2–5%; LLM generation and Q4
prefill were unchanged within noise. The main README reports much larger
gains for PyTorch FP16/BF16 workloads.
