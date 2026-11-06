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

## Config diff (prod3: 2× RTX 3090, nonguix `linux` + `nvidia-service-type`)

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

    ;; services (before nvidia-pm-set):
    (simple-service 'gpu-unlock-load shepherd-root-service-type
      (list (shepherd-service
              (provision '(gpu-unlock-load))
              (requirement '(file-systems))
              (one-shot? #t)
              (start #~(lambda _
                         (let ((modprobe (string-append #$kmod "/bin/modprobe")))
                           (and (zero? (system* modprobe "gpu_unlock"))
                                (zero? (system* modprobe "nvidia_drm"))
                                (zero? (system* modprobe "nvidia_uvm"))
                                (begin
                                  (display "gpu-unlock: NVIDIA stack loaded\n"))
                                #t))))
              (stop #~(const #f)))))

    ;; and make the power-cap service wait for it:
    ;;   (requirement '(user-processes gpu-unlock-load))  ; was '(user-processes)

## Build & deploy (prod3 recipe)

    # validate without root (channels pinned to what built the current system):
    guix time-machine -C channels.scm -- \
      system build -L /home/r/gpu-unlock/guix /home/r/gpu-unlock-port/config.scm

    # deploy (root, same pins):
    sudo guix time-machine -C channels.scm -- \
      system reconfigure -L /home/r/gpu-unlock/guix /home/r/gpu-unlock-port/config.scm

## Verify after reboot

    cat /proc/cmdline                                  # flag present
    grep -i gpu_unlock /var/log/messages               # stage=claim/gfw per GPU, before nvidia
    lsmod | grep nvidia                                # driver bound afterwards
    nvidia-smi                                         # both 3090s up

Rollback: remove `gpu_unlock.targets=all` and the nvidia blacklist
entries from `kernel-arguments` (or boot an older generation from the
GRUB menu — gpu_unlock without the cmdline flag is inert).

## channels.scm (pins used to build the current prod3 system)

    (list (channel
            (name 'guix)
            (url "https://git.guix.gnu.org/guix.git")
            (branch "master")
            (commit "df85457"))          ; per /etc/config.scm header
          (channel
            (name 'nonguix)
            (url "https://gitlab.com/nonguix/nonguix")
            (commit "c15e19cdbdfdfddacdae865741809af4fa86a665")
            (introduction
             (make-channel-introduction
              "4848788178acf1026e116b32cef8ea763133cb8a"
              (openpgp-fingerprint
               "2A39 3FFF 68F4 EF7A 3D29  12AF 6F51 20A0 22FB B2D5")))))

## Bench (post-reboot)

No torch on prod3; verification is kernel-log-based: the `gfw` stage
writes the speed selectors, and `ss0` readbacks in the log match the
unlocked profile.  For a TFLOPS number, run the 8192³ BF16 matmul
bench from the gpu-unlock skill on any box with torch + a 3090
(inf01/inf02 measured 117.3/115.2 TFLOPS vs the 75 stock cap).
