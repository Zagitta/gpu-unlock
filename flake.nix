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
      module = pkgs.linuxPackages.callPackage ./nix/package.nix { };
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

      nixosModules.default = ./nix/module.nix;
      nixosModules.gpu-unlock = self.nixosModules.default;

      checks.${system} = {
        kernel-module = module;
        firmware = module.firmware;
        nixos-integration = import ./nix/checks.nix { inherit pkgs nixpkgs; };
      } // nixpkgs.lib.optionalAttrs hasScriptedInitrd {
        nixos-integration-scripted = import ./nix/checks.nix {
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
