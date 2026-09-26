#!/bin/bash
# Assemble the AppImage exactly as upstream's .github/workflows/ci-linux.yml does.
# Runs INSIDE the ubuntu:22.04 container, after:
#   ./scripts/linux_build.sh --appimage-build --skip-cuda --sudo-off --ubuntu-test-repo --step=cmake
#   ./scripts/linux_build.sh --appimage-build --skip-cuda --sudo-off --step=build
set -euo pipefail

export https_proxy="${https_proxy:-http://127.0.0.1:7890}"
export http_proxy="${http_proxy:-http://127.0.0.1:7890}"
export APPIMAGE_EXTRACT_AND_RUN=1   # linuxdeploy's own AppImage, without FUSE
export NO_STRIP=1                   # keep the build untouched

cd /src/build
APP_ID="dev.lizardbyte.app.Sunshine"

# 1) install the build into the AppDir (ci-linux.yml step "Package Linux - AppImage")
# Start from a clean AppDir: a leftover one from an earlier run already holds the desktop file
# at AppDir/<app-id>.desktop, so the copy below then fails with "are the same file" and (under
# set -e) aborts the whole packaging step before linuxdeploy ever runs. That is exactly how a
# build whose compile succeeded produced no AppImage at all.
rm -rf AppDir
DESTDIR=AppDir ninja install
test -f AppDir/usr/share/sunshine/udev/rules.d/60-sunshine.rules && echo "udev rules: present"

# Everything below runs AppImage-packaged tools (linuxdeploy, appimagetool). In a container
# without /dev/fuse those must run unpacked, otherwise:
#   fuse: device not found, try 'modprobe fuse' first
#   Error: No suitable fusermount binary found on the $PATH
export APPIMAGE_EXTRACT_AND_RUN=1

# 2) custom AppRun + desktop file
cp -f ../packaging/linux/AppImage/AppRun ./AppDir/
chmod +x ./AppDir/AppRun
if [ -f "./AppDir/usr/share/applications/${APP_ID}.desktop" ] && \
   [ ! "./AppDir/usr/share/applications/${APP_ID}.desktop" -ef "./AppDir/${APP_ID}.desktop" ]; then
  cp -f "./AppDir/usr/share/applications/${APP_ID}.desktop" ./AppDir/
elif [ -f "../packaging/linux/AppImage/${APP_ID}.desktop" ]; then
  cp -f "../packaging/linux/AppImage/${APP_ID}.desktop" ./AppDir/
fi
ls ./AppDir/*.desktop >/dev/null 2>&1 || { echo "no desktop file for the AppImage"; exit 1; }

# 3) icon + linuxdeploy
ICON="$(find /src -maxdepth 2 -name 'sunshine.png' | head -1)"
[ -n "$ICON" ] || { echo "sunshine.png not found"; exit 1; }

for tool in linuxdeploy linuxdeploy-plugin-qt; do
  f="${tool}-x86_64.AppImage"
  if [ ! -x "$f" ]; then
    echo "== downloading $f"
    wget --max-redirect=1 -q "https://github.com/linuxdeploy/${tool}/releases/download/continuous/${f}"
    chmod +x "$f"
  fi
done

export QMAKE="${QMAKE:-/usr/lib/qt6/bin/qmake}"
export EXTRA_QT_MODULES="svg;"
[ -x "$QMAKE" ] && "$QMAKE" -query QT_INSTALL_PLUGINS || echo "note: $QMAKE missing (qt plugin step may fail)"

# Deploy into the AppDir only; the AppImage itself is built below.
./linuxdeploy-x86_64.AppImage \
  --appdir ./AppDir \
  --plugin qt \
  --executable ./sunshine \
  --icon-file "$ICON" \
  --desktop-file "$(ls ./AppDir/*.desktop | head -1)"

# appimagetool fetches its type2 runtime from GitHub releases at packaging time. On a host whose
# containers cannot reach GitHub releases (this NixOS box: cmake's downloader AND wget fail while
# curl through the proxy works) that fetch dies with
#   Failed to download runtime: server returned status code 0
#   ERROR: Failed to run plugin: appimage (exit code: 1)
# after a completely successful compile - i.e. no AppImage at all. Fetch the runtime with curl
# and hand it to appimagetool with --runtime-file instead of letting the plugin try.
RUNTIME="./runtime-x86_64"
if [ ! -s "$RUNTIME" ]; then
  echo "== fetching the AppImage type2 runtime with curl"
  curl -fL --retry 3 --max-time 300 -o "$RUNTIME" \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
fi
[ -s "$RUNTIME" ] || { echo "runtime-x86_64 unavailable"; exit 1; }

[ -d squashfs-root ] || ./linuxdeploy-x86_64.AppImage --appimage-extract >/dev/null 2>&1
AT=squashfs-root/plugins/linuxdeploy-plugin-appimage/usr/bin/appimagetool
[ -x "$AT" ] || { echo "appimagetool not found inside the linuxdeploy plugin"; exit 1; }

ARCH=x86_64 "$AT" --runtime-file "$RUNTIME" ./AppDir ./Sunshine-x86_64.AppImage

ls -l Sunshine*.AppImage
echo "APPIMAGE_OK"
