# SPDX-License-Identifier: GPL-2.0-only
{ lib, stdenv, fetchurl, python3, zstd, kernel }:
let
  version = lib.removeSuffix "\n" (builtins.readFile ../VERSION);
  # Build the source in this checkout, including downstream changes. Keep Nix
  # metadata outside the source closure so updating flake.lock alone is cheap.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../VERSION
      ../Makefile
      ../README.md
      ../kernel
      ../firmware
      ../licenses
      ../packaging
      ../tests
      ../debian
    ];
  };
  manifest = builtins.fromJSON (builtins.readFile ../firmware/artifacts-610.43.03.json);
  sourceArtifact = builtins.elemAt manifest.source_artifacts 0;
  utilsArtifact = builtins.elemAt manifest.source_artifacts 2;
  nvidiaSource = fetchurl {
    inherit (sourceArtifact) url sha256;
  };
  nvidiaUtils = fetchurl {
    inherit (utilsArtifact) url sha256;
  };
  firmware = stdenv.mkDerivation {
    pname = "gpu-unlock-firmware";
    inherit version src;
    nativeBuildInputs = [ python3 zstd ];
    buildPhase = ''
      runHook preBuild
      mkdir inputs
      ln -s ${nvidiaSource} inputs/${sourceArtifact.filename}
      ln -s ${nvidiaUtils} inputs/${utilsArtifact.filename}
      make firmware INPUTS=$PWD/inputs ASSETS=$PWD/assets
      runHook postBuild
    '';
    doCheck = true;
    checkPhase = ''
      runHook preCheck
      make test ASSETS=$PWD/assets
      runHook postCheck
    '';
    installPhase = ''
      runHook preInstall
      make install-firmware ASSETS=$PWD/assets DESTDIR=$out \
        FWDIR=/lib/firmware/gpu-unlock/610.43.03 LICENSEDIR=/share/licenses
      runHook postInstall
    '';
    dontFixup = true;
    meta = {
      description = "Pinned NVIDIA firmware and generated payload for gpu-unlock";
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
    make -C kernel/gpu_unlock KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build KCFLAGS=-Werror
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm644 kernel/gpu_unlock/gpu_unlock.ko $out/lib/modules/${kernel.modDirVersion}/extra/gpu_unlock.ko
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
