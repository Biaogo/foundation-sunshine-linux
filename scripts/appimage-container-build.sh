#!/usr/bin/env bash
# Build a portable Sunshine AppImage with the fork's own upstream build script
# (scripts/linux_build.sh) on a NixOS host, by running the whole distro lane
# inside an Ubuntu 22.04 container. Companion: scripts/make-appimage.sh (the
# in-container packaging step, transcribed from the CI's AppImage job).
#
#   scripts/appimage-container-build.sh [--keep] [--cuda] [--src <tree>]
#
# --src <tree>  source tree to build (default: the source FOD of the packaging
#               flake's pinned release, i.e. the exact released sources WITH
#               submodules — a working tree is usually missing submodule content).
# --cuda        keep CUDA enabled (the container then needs the CUDA toolkit;
#               the default CPU build is ~48 MB, a CUDA one is gigabytes).
# --keep        leave the container around for inspection.
#
# The traps handled here are documented in the skill
# sunshine-linux-hosting (rule 59 + references/appimage-foreign-distro-build.md):
# a worktree's .git is a pointer file; apt through the host proxy 502s; a rootless
# container cannot resolve DNS on this network (the host proxy can); a minimal
# image has no CA bundle (plain-http mirror first); tzdata's interactive debconf
# blocks dpkg forever; jammy's cmake 3.22 is below the required 3.24; a working
# tree has empty submodules; --appimage-build skips libva while CMake needs it;
# jammy's node v12 cannot build the web UI (and CMake uses the absolute
# /usr/bin/npm); linuxdeploy downloads hit an intermittent TLS eof.
set -euo pipefail

IMAGE="ubuntu:22.04"
NAME="fsl-appimage"
PROXY="${PROXY:-http://127.0.0.1:7890}"
MIRROR="${MIRROR:-http://mirrors.tuna.tsinghua.edu.cn/ubuntu}"
PACKAGING_NIX="${PACKAGING_NIX:-$HOME/Downloads/foundation-sunshine-linux.nix}"

REPO="$(git rev-parse --show-toplevel)"
KEEP=0
CUDA_ARG="--skip-cuda"
SRC=""

while [ $# -gt 0 ]; do
  case "$1" in
    --keep) KEEP=1; shift ;;
    --cuda) CUDA_ARG=""; shift ;;
    --src) SRC="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$SRC" ]; then
  echo "== resolving the pinned release's source tree (submodules included)"
  SRC="$(nix eval --raw "$PACKAGING_NIX#foundation-sunshine-upstream.src.outPath")"
  nix-store --realise "$SRC"
fi
[ -d "$SRC" ] || { echo "source tree not found: $SRC" >&2; exit 1; }
echo "== repo:   $REPO"
echo "== source: $SRC"

# ------------------------------------------------------------------- container
podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" --network host -v "$REPO:/repo:ro" \
  -w /src "$IMAGE" sleep infinity >/dev/null
echo "== container up"

run_in() {
  podman exec -e DEBIAN_FRONTEND=noninteractive \
    -e http_proxy="$PROXY" -e https_proxy="$PROXY" -e no_proxy=localhost,127.0.0.1 \
    -w /src "$NAME" bash -lc "$1"
}

# base tools: plain-http mirror first (no CA bundle yet), then install the CA
run_in '
set -e
sed -i "s|http://archive.ubuntu.com/ubuntu|'"$MIRROR"'|g; s|http://security.ubuntu.com/ubuntu|'"$MIRROR"'|g" /etc/apt/sources.list
printf "Acquire::http::Proxy \"'"$PROXY"'\";\nAcquire::https::Proxy \"'"$PROXY"'\";\n" > /etc/apt/apt.conf.d/99-proxy
apt-get update -qq
apt-get install -y -qq ca-certificates git curl wget xz-utils file
echo "DEBIAN_FRONTEND=noninteractive" >> /etc/environment
git config --global --add safe.directory "*"
'

# the release tree (submodules included) becomes /src
podman cp "$SRC/." "$NAME:/src/"

# cmake >= 3.24
run_in '
set -e
V=4.3.0
[ -x /opt/cmake-${V}-linux-x86_64/bin/cmake ] || {
  wget --max-redirect=1 -q -O /tmp/cmake.tar.gz "https://github.com/Kitware/CMake/releases/download/v${V}/cmake-${V}-linux-x86_64.tar.gz"
  tar -xzf /tmp/cmake.tar.gz -C /opt && rm -f /tmp/cmake.tar.gz
}
for b in cmake ctest cpack; do ln -sf /opt/cmake-${V}-linux-x86_64/bin/$b /usr/local/bin/$b; done
echo "cmake: $(cmake --version | head -1)"
'

# distribution dependencies (PPA supplies gcc-14 on jammy)
run_in "./scripts/linux_build.sh --appimage-build $CUDA_ARG --sudo-off --ubuntu-test-repo --step=deps 2>&1 | tail -5"

# libva (skipped by --appimage-build but required by CMake) + Node 22 for the web UI
run_in '
set -e
apt-get install -y -qq libva-dev libva-drm2 >/dev/null
VER=$(curl -fsSL -x '"$PROXY"' https://nodejs.org/dist/index.json | grep -o "\"version\":\"v22[^\"]*\"" | head -1 | cut -d\" -f4)
[ -x /opt/node22/bin/node ] || {
  curl -fsSL -x '"$PROXY"' -o /tmp/node.tar.xz "https://nodejs.org/dist/${VER}/node-${VER}-linux-x64.tar.xz"
  rm -rf /opt/node22 && tar -xJf /tmp/node.tar.xz -C /opt && mv /opt/node-${VER}-linux-x64 /opt/node22 && rm -f /tmp/node.tar.xz
}
for b in node npm npx; do ln -sf /opt/node22/bin/$b /usr/bin/$b; done
/usr/bin/npm config set proxy '"$PROXY"' >/dev/null
/usr/bin/npm config set https-proxy '"$PROXY"' >/dev/null
echo "node $(/usr/bin/node --version) / npm $(/usr/bin/npm --version)"
'

# configure, build, package
run_in "./scripts/linux_build.sh --appimage-build $CUDA_ARG --sudo-off --step=cmake 2>&1 | tail -4"
run_in "./scripts/linux_build.sh --appimage-build $CUDA_ARG --sudo-off --step=build > /tmp/build.log 2>&1; echo build-exit=\$?; grep -a -E 'FAILED:|error:' /tmp/build.log | head -5; tail -3 /tmp/build.log"
podman cp "$REPO/scripts/make-appimage.sh" "$NAME:/root/make-appimage.sh"
run_in 'chmod +x /root/make-appimage.sh && /root/make-appimage.sh 2>&1 | tail -12'

# ------------------------------------------------------------------- retrieve
mkdir -p "$REPO/artifacts"
APPIMAGE="$(podman exec "$NAME" bash -lc 'ls /src/build/Sunshine*.AppImage | head -1')"
podman cp "$NAME:$APPIMAGE" "$REPO/artifacts/"
ls -l "$REPO/artifacts"/Sunshine*.AppImage

[ "$KEEP" = 1 ] || podman rm -f "$NAME" >/dev/null

cat <<EOM

Built $(basename "$APPIMAGE").
Verify it on THIS host (never let the container self-certify):
  nix run nixpkgs#appimage-run -- $REPO/artifacts/$(basename "$APPIMAGE") --help
(AppRun does not intercept --version: that call would start the binary instead.)
EOM
