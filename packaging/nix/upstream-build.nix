/* Probe harness: configure/build an UPSTREAM-shaped Sunshine tree with nixpkgs deps.

   Purpose: prove that the re-fork branch (`refork/linux` = LizardByte/Sunshine upstream/master +
   this fork's Linux patch series) can be built by Nix, which is the gating question of that lane.
   Modeled on nixpkgs' `pkgs/by-name/su/sunshine/package.nix` (which packages upstream 2026.516),
   extended for the 2026-09 upstream tree (libvirtualhid/libdisplaydevice/lizardbyte-common/glad).

   Usage:
     nix-build upstream-sunshine.nix --arg configureOnly true          # deps probe only (fast)
     nix-build upstream-sunshine.nix --arg cudaSupport true -o result  # real build
*/
{ pkgs ? import <nixpkgs> { }
, srcPath ? /tmp/fsl-refork-src
, configureOnly ? false
, configureStop ? false   # full configure, no build (dependency-detection probe)
, cudaSupport ? false
}:

let
  inherit (pkgs) lib stdenv;
  inherit (stdenv.hostPlatform) isLinux;

  stdenv' = if cudaSupport then pkgs.cudaPackages.backendStdenv else stdenv;

  boost = pkgs.boost;

  # Upstream's cmake downloads prebuilt ffmpeg from LizardByte/build-deps at configure time; the
  # tag must match the pinned third-party/build-deps submodule commit (v2026.910.121303 here).
  buildDepsTag = "v2026.910.121303";
  ffmpegPrebuilt = pkgs.fetchzip {
    url = "https://github.com/LizardByte/build-deps/releases/download/${buildDepsTag}/Linux-x86_64-ffmpeg.tar.gz";
    hash = "sha256-1S57XfkJa+qEYQLmifWyT9ul0SASFhSk1lkk2timnOY=";
  };
in
stdenv'.mkDerivation (finalAttrs: {
  pname = "sunshine-refork-probe";
  version = "2026.09.25";

  src = builtins.path { path = srcPath; name = "fsl-refork-src"; };

  postPatch = ''
    # web UI is built separately; don't look for npm at configure time
    substituteInPlace cmake/targets/common.cmake \
      --replace-fail 'find_program(NPM npm REQUIRED)' ""

    # NixOS has no FHS systemd/udev discovery; the dirs are passed via cmakeFlags below
    substituteInPlace cmake/packaging/linux.cmake \
      --replace-fail 'find_package(Systemd)' "" \
      --replace-fail 'find_package(Udev)' ""
  '';

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.makeWrapper
    (pkgs.python3.withPackages (ps: [ ps.jinja2 ps.setuptools ]))  # glad generator
  ] ++ lib.optionals isLinux [
    pkgs.wayland-scanner
    pkgs.shaderc  # glslc, used at configure time
    pkgs.autoPatchelfHook
  ] ++ lib.optionals cudaSupport [
    pkgs.autoAddDriverRunpath
    pkgs.cudaPackages.cuda_nvcc
    (lib.getDev pkgs.cudaPackages.cuda_cudart)
  ];

  buildInputs = [
    boost
    boost.dev
    pkgs.curl
    pkgs.miniupnpc
    pkgs.nlohmann_json
    pkgs.openssl
    pkgs.libopus
  ] ++ lib.optionals isLinux [
    pkgs.avahi
    pkgs.libevdev
    pkgs.libpulseaudio
    pkgs.libx11
    pkgs.libxcb
    pkgs.libxfixes
    pkgs.libxrandr
    pkgs.libxtst
    pkgs.libxi
    pkgs.libdrm
    pkgs.wayland
    pkgs.libffi
    pkgs.libcap
    pkgs.pcre2
    pkgs.libuuid
    pkgs.libselinux
    pkgs.libsepol
    pkgs.libthai
    pkgs.libdatrie
    pkgs.libxdmcp
    pkgs.libxkbcommon
    pkgs.libepoxy
    pkgs.libva
    pkgs.libvdpau
    pkgs.numactl
    pkgs.libgbm
    pkgs.amf-headers
    pkgs.svt-av1
    pkgs.vulkan-loader
    pkgs.vulkan-headers
    pkgs.pipewire
    pkgs.glib          # GIO for the KWin/portal capture backends
    pkgs.libappindicator
    pkgs.libnotify
  ] ++ lib.optionals cudaSupport [
    pkgs.cudaPackages.cudatoolkit
    pkgs.cudaPackages.cuda_cudart
  ];

  cmakeFlags = [
    "-Wno-dev"
    (lib.cmakeBool "BOOST_USE_STATIC" false)
    (lib.cmakeBool "BUILD_DOCS" false)
    (lib.cmakeBool "BUILD_TESTS" false)
    (lib.cmakeBool "BUILD_WERROR" false)
    (lib.cmakeBool "GLAD_SKIP_PIP_INSTALL" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_CUDA" cudaSupport)
    (lib.cmakeBool "SUNSHINE_ENABLE_WAYLAND" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_X11" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_DRM" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_VAAPI" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_KWIN" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_PORTAL" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_VULKAN" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_TRAY" false)
    (lib.cmakeBool "SUNSHINE_SYSTEM_NLOHMANN_JSON" true)
    (lib.cmakeBool "SUNSHINE_SYSTEM_VULKAN_HEADERS" true)
    (lib.cmakeFeature "FFMPEG_PREPARED_BINARIES" "${ffmpegPrebuilt}")
    (lib.cmakeBool "UDEV_FOUND" true)
    (lib.cmakeBool "SYSTEMD_FOUND" true)
    (lib.cmakeFeature "UDEV_RULES_INSTALL_DIR" "lib/udev/rules.d")
    (lib.cmakeFeature "SYSTEMD_USER_UNIT_INSTALL_DIR" "lib/systemd/user")
    (lib.cmakeFeature "SYSTEMD_MODULES_LOAD_DIR" "lib/modules-load.d")
    (lib.cmakeFeature "SUNSHINE_EXECUTABLE_PATH" "${placeholder "out"}/bin/sunshine")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_NAME" "Biaogo")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_WEBSITE" "https://github.com/Biaogo/foundation-sunshine-linux")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_ISSUE_URL" "https://github.com/Biaogo/foundation-sunshine-linux/issues")
  ] ++ lib.optional configureOnly (lib.cmakeBool "SUNSHINE_CONFIGURE_ONLY" true);

  env = {
    BUILD_VERSION = finalAttrs.version;
    BRANCH = "master";   # build_version.cmake honours BUILD_VERSION only for "master"
    COMMIT = "refork01";
  };

  buildPhase = if (configureOnly || configureStop) then ":" else ''
    runHook preBuild
    cmake --build . --target sunshine -j $NIX_BUILD_CORES
    runHook postBuild
  '';

  installPhase = if (configureOnly || configureStop) then "mkdir -p $out" else ''
    runHook preInstall
    cmake --install .
    runHook postInstall
  '';

  postFixup = lib.optionalString (!(configureOnly || configureStop)) ''
    wrapProgram "$out/bin/sunshine" \
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.vulkan-loader ]}
  '';

  meta.mainProgram = "sunshine";
})
