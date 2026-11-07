{
  description = "Logos bitcoind_module: a Bitcoin node (Bitcoin Core), in-process.";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-bitcoin-core-nix = {
      url = "github:logos-co/logos-bitcoin-core-nix";
      inputs.logos-nix.follows = "logos-module-builder/logos-nix";
      inputs.nixpkgs.follows = "logos-module-builder/nixpkgs";
    };
    # OPTIONAL in metadata.json: only its contract is read, for the client that asks it for the SOCKS port.
    tor_module = {
      url = "github:logos-co/logos-tor-module";
      inputs.logos-module-builder.follows = "logos-module-builder";
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
      # rpc()'s allowlist and the pure halves of private sends and Tor following, compiled and run
      # on their own; and private broadcast with the module's flags, Core 31.2 in-process through
      # libbitcoind_c, under Core's own test framework: through a proxy, and through Logos' Tor.
      checks = lib.genAttrs (lib.remove "x86_64-windows" systems) (system:
        let
          pkgs = logos-module-builder.inputs.nixpkgs.legacyPackages.${system};
          core = logos-bitcoin-core-nix.packages.${system};
          privateBroadcast = mode: pkgs.runCommandCC "bitcoind-module-private-broadcast-${mode}" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -I${pkgs.nlohmann_json}/include -o print_argv tests/print_argv.cpp
            mkdir work
            python3 tests/private_broadcast_test.py --lib ${core.libbitcoind_c}/lib/libbitcoind_c${pkgs.stdenv.hostPlatform.extensions.sharedLibrary} \
              --framework ${core.bitcoin-core-src}/test/functional --print-argv ./print_argv --work $PWD/work --mode ${mode} > $out
            cat $out
          '';
        in {
          rpc-allowlist = pkgs.runCommandCC "bitcoind-module-rpc-allowlist" { } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -o allowlist tests/rpc_allowlist_test.cpp
            ./allowlist > $out
          '';
          private-sends = pkgs.runCommandCC "bitcoind-module-private-sends" { } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -I${pkgs.nlohmann_json}/include -o private_sends tests/private_sends_test.cpp
            ./private_sends > $out
          '';
          # Following Tor's SOCKS port with mocked tor_module events, and the restart's carry-over.
          tor-follow = pkgs.runCommandCC "bitcoind-module-tor-follow" { } ''
            cp -r ${./src} src
            cp -r ${./tests} tests
            $CXX -std=c++17 -Wall -Wextra -Werror -I${pkgs.nlohmann_json}/include -o tor_follow tests/tor_follow_test.cpp
            ./tor_follow > $out
          '';
          private-broadcast = privateBroadcast "proxy";
          private-broadcast-tor = privateBroadcast "tor";
        });
    };
}
