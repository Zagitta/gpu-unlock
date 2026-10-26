# Extending to a new laptop GPU

`supported_ids[]` in `kernel/gpu_unlock/main.c` matches by PCI device ID only,
with no per-board subsystem check, the same way it already does for every other
chip family. Adding a laptop/mobile die someone hasn't reported on yet is mostly
a documentation and testing exercise, not a new code path. This describes the
general procedure, then walks through it for one real board (GA107, Lenovo RTX
3050 6 GB Laptop) as a worked example.

## 1. General procedure

1. **Get the exact PCI identity.** `lspci -nnk` for the vendor:device ID and the
   subsystem vendor:device ID. The subsystem ID isn't used by `supported_ids[]`,
   but it identifies which OEM board you're reporting on.
2. **Get a VBIOS dump.** Three methods, in order of preference, since laptop
   vendors vary in which ones they allow:
   - Read the PCI expansion ROM at runtime, before any driver binds
     (`/sys/bus/pci/devices/<addr>/rom`, or `pci_map_rom`-style tooling). Some
     boards return `Input/output error` here even at full D0 power — not every
     laptop exposes FWSEC-FRTS this way.
   - Scan the OEM's latest system BIOS/UEFI image for an embedded NVIDIA
     VBIOS/GOP. Not every OEM embeds one.
   - Use the vendor's own flashing tool (e.g. `nvflash --save`) as a last
     resort. On Linux with Secure Boot, kernel lockdown's `integrity` mode blocks
     the `/dev/mem` access these tools need, with no way to disable lockdown
     alone while keeping Secure Boot on — running the same tool from a Windows
     install on the same machine (if dual-booted) sidesteps this without
     touching Secure Boot state.
3. **Parse FWSEC and diff the HAL.** Run `gpu_unlock_vbios_fwsec()`-equivalent
   parsing against the dump to get this board's own `image_off`/`size`/`imem`/
   `dmem`/`pkc` — these are board-specific, not chip-family-specific, because
   VBIOS layout differs per OEM even for the same die. Separately, diff the 5
   core HAL source files between the pinned reference tag this project builds
   from and the driver version actually installed on the test machine. An exact
   MD5 match means the pinned tag's timing/offset assumptions apply unmodified;
   a mismatch means those assumptions need re-verifying for that driver version.
4. **Confirm firmware blobs match the installed driver version.** `gsp_ga10x.bin`
   ships with the driver package itself under `/lib/firmware/nvidia/<version>/`.
   The three boot-asset binaries can be rebuilt from that driver version's own
   open-source tag and compared byte-for-byte against the pinned reference — a
   match means only the generated wrapper changed, not the decompressed
   firmware payload itself.
5. **Add the device ID, test on an isolated boot entry first.** This module has
   no dry-run/`execute=0` mode of its own — once a device ID is in
   `supported_ids[]` and `gpu_unlock.targets=` names it, the unlock pipeline runs
   for real. Add a separate GRUB/bootloader entry with the new
   `gpu_unlock.targets=<pci-addr>` cmdline so a failed attempt doesn't affect the
   normal boot entry, and read `journalctl -k -b | grep gpu_unlock` before
   trusting the result.
6. **Test both persistence paths, don't assume either.** The module unlocks a
   card twice, through two independent code paths: once at boot before `nvidia`
   binds (`unlock_one`), and again after every resume from suspend
   (`unlock_again`, already shipped — see the main README's "Boot flow and
   opt-in" section). A card that unlocks cleanly at boot can still have `nvidia`'s
   own probe-time GSP-RM re-initialization undo it before you ever get to
   benchmark; a card where that happens can still end up unlocked and stable
   after one suspend/resume cycle, since that's a different code path with
   different timing relative to GSP-RM. Verify with an MMIO readback independent
   of this module's own log messages (a trivial separate kernel module reading
   SS0/SS1/the PLM register directly is enough), and with a real throughput
   measurement, not just a register value.
7. **Watch for laptop-specific false signals.** See the gotchas list in the case
   study below — they generalize to any laptop/Optimus machine, not just this
   one board.
8. **Report it either way**, via a
   [compatibility report](https://github.com/Zagitta/gpu-unlock/issues/new?template=compatibility-report.yml)
   or a PR adding the ID, noting explicitly whether the boot-time unlock, the
   resume unlock, or both actually persisted.

## 2. Case study: GA107, Lenovo RTX 3050 6 GB Laptop

PCI `10de:25ec`, subsystem `17aa:3c90` (Lenovo board `GN20-P0-R-K2`), driver
`595.91.07`.

- **VBIOS.** Runtime PCI expansion ROM read: `Input/output error`, even at full
  D0 power — this board doesn't expose FWSEC-FRTS that way. Lenovo's latest
  signed system BIOS image for this model embeds no NVIDIA VBIOS/GOP. `nvflash
  --save` succeeded, but only from a Windows install on the same (dual-boot)
  machine — Secure Boot's kernel lockdown blocked `/dev/mem` on the Linux side.
- **FWSEC parse.** `image_off=0x41974 size=0xea00 imem=0xe200 dmem=0x800
  pkc=0x5a4` — board-specific values, different from other GA107 boards' own
  reported offsets, as expected.
- **HAL diff.** All 5 core HAL files MD5-identical between the pinned reference
  tag and `595.91.07` — no drift, no assumption needed re-verification.
- **Firmware blobs.** `gsp_ga10x.bin` for `595.91.07` ships directly under
  `/lib/firmware/nvidia/595.91.07/`. The three boot-asset binaries, rebuilt from
  the `595.91.07` open-source tag, are byte-identical to the pinned reference
  (only the generated `.c` wrapper differs). The V67 payload is a
  version-independent constant.
- **Persistence — the actual finding.** The boot-time unlock (`unlock_one`)
  writes and verifies `SS0=0x88888888` successfully, but once `nvidia.ko`'s own
  `.probe()` runs its full FWSEC→Booter→GSP-RM boot sequence right after, an
  independent MMIO readback (a small standalone diagnostic module reading
  SS0/SS1/the PLM register directly, with the card forced out of PCI runtime
  autosuspend first — see the gotcha below) shows `SS0` back at its stock value.
  This reproduced identically with the unlock module forced to run strictly
  before `udev`/any coldplug in the initramfs, ruling out a loading-order
  mistake. The resume unlock (`unlock_again`, this module's own existing
  mechanism — its `PM_POST_SUSPEND` notifier at priority 1 runs ahead of
  `nvidia`'s own resume notifier at the default priority 0) behaves differently:
  after one `systemctl suspend`/resume cycle with `nvidia` already bound and
  running normally, the independent MMIO readback shows `PLM=0xffffffff
  SS0=0x88888888 SS1=0x8`, still present after a second suspend/resume cycle
  (the module logs `-EALREADY` the second time — it never had to rewrite
  anything). A real CUDA microbenchmark (dense register MMA, SM clock locked at
  855 MHz) confirms it: fp16/bf16 measured 1019.5-1022.6 ops/SM/clock against a
  stock baseline of 511.7, matching the expected doubled rate; tf32 is
  unaffected (~510), consistent with only fp16/bf16 going through this
  selector. On this board, the resume path is what makes the unlock usable —
  the boot-time path alone is not enough.
- **Resume success itself is conditional, not guaranteed.** The resume
  re-unlock above doesn't win every time — it's a real race against
  `nvidia`'s own `PM_POST_SUSPEND` resume notifier, and priority ordering
  alone didn't settle it in practice. Automating `systemctl suspend` (via
  `rtcwake`, scripted, no real desktop activity beforehand) lost this race
  consistently — every single attempt, across dozens of trials, whether
  triggered right at the login screen, after a fixed delay up to ~90 seconds
  post-login, or immediately following an automated attempt to generate GPU
  load. Suspending by hand after ~5 minutes of simply being logged in (no
  deliberate activity) won immediately; suspending after as little as ~60
  seconds of genuine desktop use (moving windows, video playback) also won
  immediately. The exact trigger isn't pinned down — accumulated real
  GPU/display activity and a longer idle threshold than any scripted retry
  loop was given are both still candidates — but on this board, "retry a
  plain suspend/resume a few times right after boot" does not reliably work;
  only a deliberately delayed or human-driven suspend has, so far.
- **Laptop-specific gotchas hit while measuring this:**
  - Booting with the unlock module active changed which display scale GDM
    picked (100% instead of the usual 125%) — GDM's own `mutter` instance
    enumerates GPUs differently depending on whether `nvidia` is already active
    at GDM startup. An Optimus/dual-GPU desktop-environment effect, unrelated to
    this module.
  - An MMIO readback where SS0, SS1 and the PLM register all come back
    `0xffffffff` together almost always means the card is in PCI runtime
    autosuspend (D3cold), not that the unlock was undone. Check
    `/sys/bus/pci/devices/<addr>/power/runtime_status` and force
    `power/control=on` before trusting a read like that.
  - Manually `modprobe`ing `nvidia` outside the normal boot sequence can leave
    `nvidia-powerd.service` (Dynamic Boost) not auto-started, silently capping
    the card at its default power limit instead of its dynamic ceiling — this
    quietly understates any throughput comparison taken under those conditions.
  - A BIOS/MUX switch to discrete-GPU-only mode does not address this at all —
    it changes display routing, not the GSP-RM re-initialization behavior this
    depends on — and carries at least one documented report of leaving a similar
    laptop with a black screen until a BIOS defaults reset. Not a workaround for
    this.
  - Baking the cold-boot step into the real default boot (instead of a
    one-off GRUB test entry) needs whichever initramfs generator the system
    actually uses for its default images, not just whichever one happens to
    also be installed: on this Ubuntu install, `/usr/sbin/update-initramfs`
    is dpkg-diverted to dracut, not `initramfs-tools`, even though
    `initramfs-tools-core` is also installed and its own `mkinitramfs` works
    fine when invoked directly (`dpkg -S $(readlink -f $(which
    update-initramfs))` tells you which one actually owns it). For dracut,
    `force_drivers+=" <module> "` in `/etc/dracut.conf.d/` plus
    `install_items+=` for the firmware is the equivalent of
    initramfs-tools' `modules.d` hook — dracut's own `rd.driver.pre=`
    mechanism (set automatically by `force_drivers`) loads it ahead of udev
    on its own, no custom ordering hook needed.
- **Real-model benchmark** (transformer + CNN train/inference, not just the
  register microbenchmark above): confirms it holds up outside a synthetic
  register-only kernel, at a lower margin than the raw ~2x instruction-issue
  rate, as expected once memory bandwidth and non-tensor-core work dilute it.
  `fp32` (no tensor cores, the control) is flat on both models, as it should
  be if this is really a tensor-core effect and not a general clock/power
  change:

  | Model | Precision | Train | Inference |
  | --- | --- | ---: | ---: |
  | Transformer | fp32 (control) | +2.7% | +3.8% |
  | Transformer | tf32 | +30.0% | +46.3% |
  | Transformer | fp16 (amp) | +21.7% | +31.3% |
  | Transformer | bf16 (amp) | +21.4% | +32.2% |
  | CNN (ResNet-18) | fp32 (control) | -0.0% | -0.1% |
  | CNN (ResNet-18) | tf32 | +13.8% | +17.7% |
  | CNN (ResNet-18) | fp16 (amp) | +17.9% | +17.3% |
  | CNN (ResNet-18) | bf16 (amp) | +17.2% | +18.6% |

  The transformer (GEMM-dominated, synthetic tokens) shows a cleaner, larger
  gain than the CNN (cuDNN picks the conv algorithm per layer, not every
  shape is tensor-core shaped) — the same pattern the register microbenchmark
  predicted.
- **Prior art.** [`IHaBiS02/ga107-unlock`](https://github.com/IHaBiS02/ga107-unlock)
  independently targets a different GA107 board (LG, subsystem `1854:052f`) as a
  single-board-hardcoded fork of an earlier version of this module. This entry
  instead extends the existing generic `supported_ids[]` mechanism, which
  already handles board-to-board VBIOS differences at runtime without a
  dedicated fork per board.
