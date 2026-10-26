# Nix flake and NixOS module

The flake builds the module from **its own checkout**, not a separately pinned
copy of the upstream repository. Firmware inputs and extracted/generated
outputs remain SHA-256 pinned by `firmware/artifacts-610.43.03.json`.
Builds and checks never load a kernel module or access a GPU.

## NixOS installation

Add the flake input to your system flake. A `github:` input records a specific
commit in your system's `flake.lock`; commit that lock file for reproducibility.

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    gpu-unlock.url = "github:Zagitta/gpu-unlock";
    gpu-unlock.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, gpu-unlock, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        gpu-unlock.nixosModules.default
        ({ lib, ... }: {
          # Integrate this with any existing allowUnfree policy.
          nixpkgs.config.allowUnfreePredicate = pkg:
            lib.getName pkg == "gpu-unlock-firmware";

          hardware.nvidia.gpuUnlock = {
            enable = true;
            targets = [ "0000:01:00.0" ]; # use your lspci -D address
            resume = false;
          };
        })
      ];
    };
  };
}
```

Configure NVIDIA normally in `configuration.nix`. Its driver and CUDA
configuration are not replaced by this module. If that configuration also
needs non-free packages, extend your existing allowlist or use the unfree
policy you already maintain; do not replace it with a narrower list blindly.

The module is disabled by default and requires an explicit nonempty target
list when enabled. `targets = [ "all" ];` is also supported, but `all` cannot
be mixed with PCI addresses. The module deliberately defaults `resume` to
false: reapplying after suspend depends on NVIDIA suspend-notifier ordering
and must be validated separately on the target host.

Build first, then prepare a boot generation:

```sh
nix build .#nixosConfigurations.myhost.config.system.build.toplevel
sudo nixos-rebuild boot --flake .#myhost
```

The latter updates the boot menu; it does not reboot. Plan the first boot with
a working recovery entry and local access. Removing the module's `enable`
setting and rebuilding removes its module, firmware dependency, and kernel
parameters from subsequent generations. Keep a known-good generation for
rollback; hardware/firmware failures may require a cold power cycle.

## Outputs and custom kernels

Only `x86_64-linux` is currently exposed and tested.

| Output | Purpose |
| --- | --- |
| `packages.x86_64-linux.default` / `gpu-unlock` | Module for the flake's default Linux kernel |
| `packages.x86_64-linux.gpu-unlock-firmware` | The five pinned firmware/payload files and license notices |
| `nixosModules.default` / `gpu-unlock` | Opt-in NixOS integration |
| `checks.x86_64-linux` | Kernel build, firmware tests, actual initrd integration |
| `devShells.x86_64-linux.default` | Python, zstd, make, GCC, Git |

```sh
nix build github:Zagitta/gpu-unlock#gpu-unlock
nix build github:Zagitta/gpu-unlock#gpu-unlock-firmware
nix flake check --print-build-logs
nix develop
```

The flake package outputs allow precisely `gpu-unlock-firmware` under their
own nixpkgs import, so a global `NIXPKGS_ALLOW_UNFREE` override is unnecessary.
The NixOS module uses **your** package set and its license policy.

The NixOS module's default package comes from
`config.boot.kernelPackages.callPackage ./package.nix { }`, so changing
`boot.kernelPackages` selects the matching module headers automatically.
Do not install the standalone default-kernel `.ko` into an unrelated kernel.
An explicit override is available when a caller needs one:

```nix
hardware.nvidia.gpuUnlock.package =
  gpu-unlock.packages.x86_64-linux.gpu-unlock.override {
    kernel = config.boot.kernelPackages.kernel;
  };
```

## Boot ordering and validation

For a systemd initrd, a oneshot loads `gpu_unlock` before
`systemd-modules-load.service`, `systemd-udevd.service`, and
`systemd-udev-trigger.service`. For a scripted initrd, the module is placed at
the front of the synchronous kernel-module list, before udev starts. The
firmware is included through the module's `MODULE_FIRMWARE` declarations.
Loading the module after NVIDIA binds is not an unlock test.

`nix flake check` builds the module with `KCFLAGS=-Werror`, generates the
firmware and runs the upstream component/package tests with the real pinned
assets against mocked hardware. Its NixOS check also exercises opt-out and
invalid target handling, builds/unpacks an initrd, verifies kernel ABI and
all five firmware hashes, and checks both systemd and scripted initrd loading order.
These checks require no GPU and do not prove a particular board works.

After boot, inspect:

```sh
journalctl -k -b | grep -E 'gpu_unlock|FWSEC|selector|FLR|NVRM|Xid'
lspci -nnk -s 0000:01:00.0
nvidia-smi
```

## Licenses

The module and Nix additions are GPL-2.0-only. The generated payload and
NVIDIA firmware retain their respective upstream terms; see
[`firmware/NOTICE.md`](../firmware/NOTICE.md), `licenses/`, and the firmware
package's installed NVIDIA license. No NVIDIA binary is committed to this
repository. This tool does not increase the GPU's power limit or VRAM.
