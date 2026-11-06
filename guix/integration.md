# gpu-unlock on Guix System (nonguix)

Port of Zagitta/gpu-unlock for Guix. The Arch/Debian/Fedora install
paths don't apply; on Guix the module is packaged natively and loaded
at the rootfs stage, which avoids touching the initrd at all.

## Design

- `(make-gpu-unlock-module linux)` — the out-of-tree module, built with
  `linux-module-build-system` against the EXACT kernel package the
  system config uses (`linux` from nonguix, not `linux-libre`).
- `gpu-unlock-firmware` — the 16 asset files the module requests from
  `lib/firmware/gpu-unlock/610.43.03/`, extracted from NVIDIA
  610.43.03 by the repo's `firmware/extract_gsp.py` +
  `firmware/extract_boot_assets.py` (needs `zstd` in PATH).
- nonguix loads nvidia purely via udev coldplug (no shepherd loader).
  The unlock must run first, so: blacklist `nvidia*` on the kernel
  command line (kills udev auto-load), then a shepherd one-shot
  explicitly loads `gpu_unlock` and then the NVIDIA driver stack.
  Deterministic order, no initrd involvement, firmware resolves from
  the real root filesystem.
- The module never registers as a PCI driver (no MODULE_DEVICE_TABLE),
  so it can never race udev for the device.

## Config additions (nonguix `linux` + `nvidia-service-type`)

Imports:

    (gpu-unlock)          ;; via `guix system ... -L <repo>/guix`
    (gnu packages linux)  ;; kmod
    (gnu services)        ;; simple-service, shepherd-root-service-type

operating-system additions:

    (kernel-loadable-modules (list (make-gpu-unlock-module linux)))

    (firmware (cons* gpu-unlock-firmware
                     linux-firmware
                     %base-firmware))

    (kernel-arguments (cons*
                       "modprobe.blacklist=nouveau,nvidia,nvidia_modeset,nvidia_uvm,nvidia_drm"
                       "gpu_unlock.targets=all"
                       %default-kernel-arguments))

    ;; the boot-time one-shot is too long to inline here — see
    ;; config-example.scm in this directory for the complete,
    ;; boot-proven service (holder sweep, udev freeze, retry logic,
    ;; modprobe -d).  Its four essential steps:
    ;;   1. stop nvidia-powerd (shepherd-respawned daemons must be
    ;;      stopped via herd, and powerd holds the driver open)
    ;;   2. freeze the udev exec queue (nonguix's setuid nvidia-modprobe
    ;;      udev rule would otherwise rebind mid-eject)
    ;;   3. sweep /dev/nvidia* fd holders, eject the bound stack, load
    ;;      gpu_unlock, resume udev, then modprobe the NVIDIA stack with
    ;;      -d /run/booted-system/kernel
    ;;   4. require 'gpu-unlock-load in any service that opens CUDA and
    ;;      in the nvidia-pm-set service, or they hold the driver and
    ;;      the unlock silently no-ops

## Build & deploy

    # validate without root (channels pinned to what built the current system):
    guix time-machine -C channels.scm -- \
      system build -L <repo>/guix path/to/your/config.scm

    # deploy (root, same pins):
    sudo guix time-machine -C channels.scm -- \
      system reconfigure -L <repo>/guix path/to/your/config.scm

## Verify after reboot

    cat /proc/cmdline                                  # flag present
    grep -i gpu_unlock /var/log/messages               # stage=claim/gfw per GPU, before nvidia
    lsmod | grep nvidia                                # driver bound afterwards
    nvidia-smi                                         # both 3090s up

Rollback: remove `gpu_unlock.targets=all` and the nvidia blacklist
entries from `kernel-arguments` (or boot an older generation from the
GRUB menu — gpu_unlock without the cmdline flag is inert).

## channels.scm (pin the pair that built your current system)

    Read /run/current-system/provenance to see the commit pair your
    running system was built from, and pin both.  Pinned commits need
    explicit introductions (see "channel authentication" in the Guix
    manual) — a seeded or older cache otherwise fails with
    "channel 'guix' lacks 'introduction' field but '.guix-authorizations'
    found".  If a box's outbound git path is broken, pre-seed
    ~/.cache/guix/checkouts/ from a box that resolved the identical
    spec (slot names are not derivable from the URL — seed every
    observed name).

## Bench (post-reboot)

Verified on 3 Guix hosts / 2 kernel versions (7.1.8, 7.2.15), 2× RTX
3090 each.  Boot-persistent unlock, full log sequence, zero errors:

    GPU0 117.8 TFLOPS / GPU1 113.0 TFLOPS   (8192³ BF16, torch bench)
    vs 70.1 TFLOPS stock                     (~1.7×)

Quick check without torch: `dmesg | grep gpu_unlock` shows the
stage=claim/gfw lines with ss0=0x88888888 per GPU before the driver
binds, and /run/gpu-unlock-load.log carries the one-shot's sequence.
