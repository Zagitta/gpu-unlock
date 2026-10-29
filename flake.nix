{
  description = "Ampere GPU unlock: kernel module, pinned firmware, and NixOS integration";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        # Only the explicitly requested firmware package needs this exception.
        config.allowUnfreePredicate = pkg: nixpkgs.lib.getName pkg == "gpu-unlock-firmware";
      };
      module = pkgs.linuxPackages.callPackage ./packaging/nix/package.nix { };
      nixosOptions = (nixpkgs.lib.nixosSystem {
        inherit system;
        modules = [{ nixpkgs.pkgs = pkgs; }];
      }).options;
      hasScriptedInitrd = (nixosOptions.boot.initrd.systemd.enable.type.name or null) == "bool";
    in
    {
      packages.${system} = {
        default = module;
        gpu-unlock = module;
        gpu-unlock-firmware = module.firmware;
      };

      nixosModules.default = ./packaging/nix/module.nix;
      nixosModules.gpu-unlock = self.nixosModules.default;

      checks.${system} = {
        kernel-module = module;
        firmware = module.firmware;
        packaging-hashes =
          let
            lib = nixpkgs.lib;
            pkgbuild = builtins.readFile ./packaging/arch/PKGBUILD;
            spec = builtins.readFile ./packaging/rpm/gpu-unlock.spec;
            makefile = builtins.readFile ./Makefile;
          in
          assert lib.hasInfix module.nvidiaVersion pkgbuild;
          assert lib.hasInfix module.nvidiaVersion spec;
          assert lib.hasInfix module.nvidiaVersion makefile;
          assert lib.hasInfix module.nvidiaSourceSha256 pkgbuild;
          assert lib.hasInfix module.nvidiaSourceSha256 spec;
          assert lib.hasInfix module.nvidiaInstallerSha256 pkgbuild;
          assert lib.hasInfix module.nvidiaInstallerSha256 spec;
          pkgs.runCommand "gpu-unlock-packaging-hashes" { } ''
            echo 'NVIDIA version and sha256 values match PKGBUILD, RPM spec, and Makefile' > $out
          '';
        nixos-integration = import ./packaging/nix/checks.nix { inherit pkgs nixpkgs; };
      } // nixpkgs.lib.optionalAttrs hasScriptedInitrd {
        nixos-integration-scripted = import ./packaging/nix/checks.nix {
          inherit pkgs nixpkgs;
          systemdInitrd = false;
        };
      };

      formatter.${system} = pkgs.nixpkgs-fmt;
      devShells.${system}.default = pkgs.mkShell {
        packages = [ pkgs.python3 pkgs.zstd pkgs.gnumake pkgs.gcc pkgs.git ];
      };
    };
}
