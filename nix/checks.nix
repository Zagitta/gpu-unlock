# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, nixpkgs, systemdInitrd ? true }:
let
  lib = nixpkgs.lib;
  makeSystem = enable: targets: nixpkgs.lib.nixosSystem {
    system = "x86_64-linux";
    modules = [
      ./module.nix
      {
        nixpkgs.pkgs = pkgs;
        system.stateVersion = "26.05";
        boot.loader.grub.enable = false;
        fileSystems."/" = { device = "none"; fsType = "tmpfs"; };
        hardware.cpu.intel.updateMicrocode = false;
        hardware.cpu.amd.updateMicrocode = false;
        hardware.nvidia.gpuUnlock = { inherit enable targets; };
        boot.initrd.systemd.enable = systemdInitrd;
      }
    ];
  };
  enabled = (makeSystem true [ "0000:01:00.0" ]).config;
  disabled = (makeSystem false [ ]).config;
  invalid = (makeSystem true [ ]).config;
  mixed = (makeSystem true [ "all" "0000:01:00.0" ]).config;
  assertionsPass = cfg: builtins.all (a: a.assertion) cfg.assertions;
in
assert assertionsPass enabled;
assert !assertionsPass invalid;
assert !assertionsPass mixed;
assert !(builtins.elem "gpu_unlock" disabled.boot.initrd.kernelModules);
assert !(builtins.hasAttr "gpu-unlock" disabled.boot.initrd.systemd.services);
assert builtins.filter (lib.hasPrefix "gpu_unlock.") disabled.boot.kernelParams == [ ];
assert enabled.system.build.gpuUnlock.kernelVersion == enabled.boot.kernelPackages.kernel.modDirVersion;
pkgs.runCommand "gpu-unlock-nixos-integration-${if systemdInitrd then "systemd" else "scripted"}"
{
  nativeBuildInputs = [ pkgs.python3 pkgs.cpio pkgs.zstd pkgs.kmod ];
}
  ''
    mkdir root
    (cd root; zstd -dc ${enabled.system.build.initialRamdisk}/initrd | cpio --quiet -id --no-absolute-filenames)
    python3 ${./check-initrd.py} root ${enabled.system.build.gpuUnlock.firmware} \
      ${../firmware/artifacts-610.43.03.json} ${enabled.boot.kernelPackages.kernel.modDirVersion} \
      ${if systemdInitrd then "systemd" else "scripted"}
    mkdir -p $out
    cp verified-initrd-ordering.txt $out/
    echo 'module opt-in, target validation, kernel ABI, firmware hashes, and initrd ordering: PASS' > $out/result
  ''
