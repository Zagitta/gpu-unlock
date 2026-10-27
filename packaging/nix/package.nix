# SPDX-License-Identifier: GPL-2.0-only
{ lib, stdenv, fetchurl, python3, zstd, kernel, kernelModuleMakeFlags }:
let
  version = lib.removeSuffix "\n" (builtins.readFile ../../VERSION);
  nvidiaVersion = "610.43.03";
  kernelBuild = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";
  kbuildFlags = lib.escapeShellArgs kernelModuleMakeFlags;
  # Build the source in this checkout, including downstream changes. Keep Nix
  # metadata outside the source closure so updating flake.lock alone is cheap.
  src = lib.fileset.toSource {
    root = ../../.;
    fileset = lib.fileset.unions [
      ../../VERSION
      ../../Makefile
      ../../README.md
      ../../kernel
      ../../firmware
      ../../licenses
      (lib.fileset.difference ../../packaging ./.)
      ../../debian
    ];
  };
  nvidiaSource = fetchurl {
    url = "https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/NVIDIA-kernel-module-source-${nvidiaVersion}.tar.xz";
    sha256 = "7e118923c7a23edc36114d63273a46e3e04e9af98695a42203e7ac2dfe9fc1dc";
  };
  nvidiaInstaller = fetchurl {
    url = "https://download.nvidia.com/XFree86/Linux-x86_64/${nvidiaVersion}/NVIDIA-Linux-x86_64-${nvidiaVersion}-no-compat32.run";
    sha256 = "e084451db730a6df29d3d74f655749c4d236e3c945f7d94b5205e84318fe2f90";
  };
  firmware = stdenv.mkDerivation {
    pname = "gpu-unlock-firmware";
    inherit version src;
    nativeBuildInputs = [ python3 zstd ];
    buildPhase = ''
      runHook preBuild
      mkdir inputs
      ln -s ${nvidiaSource} inputs/NVIDIA-kernel-module-source-${nvidiaVersion}.tar.xz
      ln -s ${nvidiaInstaller} inputs/NVIDIA-Linux-x86_64-${nvidiaVersion}-no-compat32.run
      make firmware INPUTS=$PWD/inputs ASSETS=$PWD/assets
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      make install-firmware ASSETS=$PWD/assets DESTDIR=$out \
        FWDIR=/lib/firmware/gpu-unlock/${nvidiaVersion} LICENSEDIR=/share/licenses
      runHook postInstall
    '';
    dontFixup = true;
    meta = {
      description = "Pinned NVIDIA firmware for gpu-unlock";
      license = [ lib.licenses.unfreeRedistributableFirmware lib.licenses.gpl2Only ];
      platforms = [ "x86_64-linux" ];
    };
  };
in
stdenv.mkDerivation {
  pname = "gpu-unlock";
  inherit version src;
  nativeBuildInputs = kernel.moduleBuildDependencies;
  hardeningDisable = [ "pic" ];
  buildPhase = ''
    runHook preBuild
    make -C ${kernelBuild} M=$PWD/kernel/gpu_unlock ${kbuildFlags} KCFLAGS=-Werror modules
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    # Use Kbuild installation so nixpkgs signing/certificate flags are honored.
    make -C ${kernelBuild} M=$PWD/kernel/gpu_unlock ${kbuildFlags} \
      INSTALL_MOD_PATH=$out INSTALL_MOD_DIR=extra DEPMOD=true modules_install
    install -Dm644 README.md $out/share/doc/gpu-unlock/README.md
    install -Dm644 licenses/GPL-2.0.txt $out/share/licenses/gpu-unlock/GPL-2.0.txt
    runHook postInstall
  '';
  # Kernel objects must not go through user-space ELF/RPATH fixups.
  dontFixup = true;
  passthru = { inherit firmware; kernelVersion = kernel.modDirVersion; };
  meta = {
    description = "Ampere pre-driver Tensor throughput unlock";
    homepage = "https://github.com/Zagitta/gpu-unlock";
    license = lib.licenses.gpl2Only;
    platforms = [ "x86_64-linux" ];
  };
}
