# Nix / NixOS

Supports `x86_64-linux`. Firmware inputs and outputs are SHA-256 pinned by
[`artifacts-610.43.03.json`](../firmware/artifacts-610.43.03.json).

## NixOS

Add the input to your system flake:

```nix
inputs.gpu-unlock.url = "github:Zagitta/gpu-unlock";
inputs.gpu-unlock.inputs.nixpkgs.follows = "nixpkgs";
```

Include `gpu-unlock` in your `outputs` arguments and add the module to your host:

```nix
modules = [
  ./configuration.nix
  gpu-unlock.nixosModules.default
  ({ lib, ... }: {
    # Extend your existing unfree policy if you already have one.
    nixpkgs.config.allowUnfreePredicate = pkg:
      lib.getName pkg == "gpu-unlock-firmware";

    hardware.nvidia.gpuUnlock = {
      enable = true;
      targets = [ "0000:01:00.0" ]; # your address from lspci -D
    };
  })
];
```

Keep your existing NVIDIA configuration. The module builds against
`boot.kernelPackages` and loads before NVIDIA in either a systemd or scripted
initrd. It defaults to disabled and requires explicit targets;
`targets = [ "all" ];` selects all supported cards. `resume` defaults to false
and requires separate validation of NVIDIA suspend-notifier ordering.

```sh
sudo nixos-rebuild boot --flake .#myhost
```

Reboot when ready, keeping a known-good generation available for recovery.
After boot, check:

```sh
journalctl -k -b | grep -E 'gpu_unlock|FWSEC|selector|FLR|NVRM|Xid'
nvidia-smi
```

## Standalone builds and checks

```sh
nix build .#gpu-unlock           # module for the flake's default kernel
nix build .#gpu-unlock-firmware
nix flake check --print-build-logs
nix develop
```

Run these from the checkout. The flake permits its firmware package's unfree
license; the NixOS module follows your system's license policy.

Checks compile the module, run the upstream tests, and verify the module ABI,
firmware hashes, and loading order in the initrd formats supported by NixOS. CI
covers both the locked nixpkgs and unstable. Checks require no GPU
and do not validate hardware compatibility or performance.

See [`firmware/NOTICE.md`](../firmware/NOTICE.md) and `licenses/` for license terms.
