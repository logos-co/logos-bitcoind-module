{
  description = "Logos bitcoind_module: a Bitcoin node (Bitcoin Core), in-process.";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-bitcoin-core-nix = {
      url = "github:logos-co/logos-bitcoin-core-nix";
      inputs.logos-nix.follows = "logos-module-builder/logos-nix";
      inputs.nixpkgs.follows = "logos-module-builder/nixpkgs";
    };
  };

  outputs = inputs@{ logos-module-builder, logos-bitcoin-core-nix, ... }:
    let
      lib = logos-module-builder.inputs.nixpkgs.lib;
      # x86_64-windows is a cross build from x86_64-linux.
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" "x86_64-windows" ];
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs.bitcoind_c = {
          input = logos-bitcoin-core-nix;
          packages.default = "libbitcoind_c";
        };
      };
    in
    {
      packages = lib.genAttrs systems (system: module.packages.${system});
      # rpc()'s allowlist, compiled and run on its own; and private broadcast with the module's
      # flags, Core 31.1 in-process through libbitcoind_c, under Core's own test framework.
      checks = lib.genAttrs (lib.remove "x86_64-windows" systems) (system:
        let
          pkgs = logos-module-builder.inputs.nixpkgs.legacyPackages.${system};
          core = logos-bitcoin-core-nix.packages.${system};
        in {
          rpc-allowlist = pkgs.runCommandCC "bitcoind-module-rpc-allowlist" { } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -o allowlist tests/rpc_allowlist_test.cpp
            ./allowlist > $out
          '';
          private-broadcast = pkgs.runCommandCC "bitcoind-module-private-broadcast" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -I${pkgs.nlohmann_json}/include -o print_argv tests/print_argv.cpp
            mkdir work
            python3 tests/private_broadcast_test.py --lib ${core.libbitcoind_c}/lib/libbitcoind_c${pkgs.stdenv.hostPlatform.extensions.sharedLibrary} \
              --framework ${core.bitcoin-core-src}/test/functional --print-argv ./print_argv --work $PWD/work > $out
            cat $out
          '';
        });
    };
}
