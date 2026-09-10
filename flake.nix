{
  description = "Resilient Guitarix DSP supervisor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          guitarixDsp = pkgs.guitarix.overrideAttrs (prev: {
            patches = (prev.patches or [ ]) ++ [
              ./patches/guitarix-dsp-wscript.patch
              ./patches/guitarix-dsp-noise-gate.patch
            ];
            postPatch = (prev.postPatch or "") + ''
              cp ${./dsp/guitar-rack-dsp.cpp} src/gx_head/guitar-rack-dsp.cpp
            '';
          });
        in {
          guitar-rack-dsp = guitarixDsp;
          default = pkgs.rustPlatform.buildRustPackage {
            pname = "guitar-rack";
            version = "0.1.0";
            src = pkgs.lib.cleanSource ./.;
            cargoLock.lockFile = ./Cargo.lock;

            nativeBuildInputs = with pkgs; [
              makeWrapper
              pkg-config
              kdePackages.wrapQtAppsHook
            ];
            buildInputs = with pkgs; [
              kdePackages.qtbase
            ];

            postInstall = ''
              install -Dm644 assets/guitar-rack.svg \
                $out/share/icons/hicolor/scalable/apps/guitar-rack.svg
              install -Dm644 /dev/stdin $out/share/applications/guitar-rack.desktop <<'DESKTOP'
              [Desktop Entry]
              Type=Application
              Name=Guitar Rack
              Comment=Resilient Guitarix guitar effects engine
              Exec=guitar-rack
              Icon=guitar-rack
              Terminal=false
              Categories=Audio;AudioVideo;Music;
              DESKTOP
            '';

            postFixup = ''
              wrapProgram $out/bin/guitar-rack \
                --set QT_STYLE_OVERRIDE Fusion
              wrapProgram $out/bin/guitar-rack-engine \
                --prefix LD_LIBRARY_PATH : ${pkgs.lib.makeLibraryPath [ pkgs.pipewire.jack ]} \
                --set GUITAR_RACK_DSP ${guitarixDsp}/bin/guitar-rack-dsp
            '';
          };
        });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          guitarixDsp = pkgs.guitarix.overrideAttrs (prev: {
            patches = (prev.patches or [ ]) ++ [
              ./patches/guitarix-dsp-wscript.patch
              ./patches/guitarix-dsp-noise-gate.patch
            ];
            postPatch = (prev.postPatch or "") + ''
              cp ${./dsp/guitar-rack-dsp.cpp} src/gx_head/guitar-rack-dsp.cpp
            '';
          });
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [ cargo clippy pkg-config rustc rustfmt ];
            buildInputs = with pkgs; [ kdePackages.qtbase ];
            GUITAR_RACK_DSP = "${guitarixDsp}/bin/guitar-rack-dsp";
            shellHook = ''
              export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [ pkgs.pipewire.jack ]}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
            '';
          };
        });
    };
}
