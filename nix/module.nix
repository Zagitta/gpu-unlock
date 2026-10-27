# SPDX-License-Identifier: GPL-2.0-only
{ config, lib, options, pkgs, ... }:
let
  cfg = config.hardware.nvidia.gpuUnlock;
  # Newer NixOS releases retain a throwing removed-option stub. Inspect the
  # declaration rather than evaluating that removed configuration value.
  hasInitrdToggle = (options.boot.initrd.systemd.enable.type.name or null) == "bool";
  systemdInitrd = !hasInitrdToggle || config.boot.initrd.systemd.enable;
  pciTarget = lib.types.strMatching "(all|[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\\.[0-7])";
in
{
  options.hardware.nvidia.gpuUnlock = {
    enable = lib.mkEnableOption "experimental Ampere Tensor throughput unlocking before NVIDIA binds";

    package = lib.mkOption {
      type = lib.types.package;
      default = config.boot.kernelPackages.callPackage ./package.nix { };
      defaultText = lib.literalExpression "config.boot.kernelPackages.callPackage ./package.nix { }";
      description = "Kernel module package, built for the configured kernel, with firmware in passthru.";
    };

    targets = lib.mkOption {
      type = lib.types.listOf pciTarget;
      default = [ ];
      example = [ "0000:01:00.0" ];
      description = "Explicit PCI addresses to unlock, or a single all entry for every supported card.";
    };

    resume = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Reapply after suspend. Requires validated NVIDIA suspend-notifier ordering.";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = cfg.targets != [ ] && (!(builtins.elem "all" cfg.targets) || cfg.targets == [ "all" ]);
        message = "hardware.nvidia.gpuUnlock.targets must contain PCI addresses or only the single all entry.";
      }
      {
        assertion = pkgs.stdenv.hostPlatform.system == "x86_64-linux";
        message = "The pinned gpu-unlock firmware package is currently validated only on x86_64-linux.";
      }
    ];

    boot.extraModulePackages = [ cfg.package ];
    hardware.firmware = [ cfg.package.firmware ];
    # Scripted stage 1 loads this synchronously before starting udev.
    boot.initrd.kernelModules = lib.mkBefore [ "gpu_unlock" ];
    # For systemd stage 1, finish before coldplug or explicit modules can bind NVIDIA.
    boot.initrd.systemd.services.gpu-unlock = lib.mkIf systemdInitrd {
      description = "Unlock Ampere Tensor rate before NVIDIA probes";
      wantedBy = [ "sysinit.target" ];
      before = [ "systemd-modules-load.service" "systemd-udevd.service" "systemd-udev-trigger.service" ];
      unitConfig.DefaultDependencies = false;
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = "${pkgs.kmod}/bin/modprobe gpu_unlock";
      };
    };
    boot.initrd.systemd.storePaths = lib.mkIf systemdInitrd [ "${pkgs.kmod}/bin/modprobe" ];
    boot.kernelParams = [
      "gpu_unlock.targets=${lib.concatStringsSep "," cfg.targets}"
      "gpu_unlock.resume=${if cfg.resume then "1" else "0"}"
    ];
    system.build.gpuUnlock = cfg.package;
    system.build.gpuUnlockFirmware = cfg.package.firmware;
  };
}
