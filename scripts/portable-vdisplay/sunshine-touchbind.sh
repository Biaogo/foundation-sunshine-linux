#!/usr/bin/env bash
# Sunshine touch-binding waiter — portable (no Nix) build.
#
# KWin binds absolute-input devices (touchscreen, pen tablet) to an output via
# the device's `outputName` DBus property.  The devices Sunshine creates
# (libvirtualhid) appear only once the client connects — i.e. after the do-hook
# returned — and they come up with `outputName` EMPTY, in which case KWin
# silently drops every touch event (the cursor stays in the corner) or maps it
# to the wrong output.
#
# This helper polls the input devices for up to ~2 minutes and points them at
# the given output.
#
# Usage: sunshine-touchbind.sh <kwin-output-name>

set -u

if [ $# -lt 1 ]; then
  echo "usage: sunshine-touchbind.sh <kwin-output-name>" >&2
  exit 2
fi

OUT="$1"
LOG="${SUNSHINE_VDISPLAY_LOG:-${XDG_RUNTIME_DIR:-/tmp}/sunshine-vdisplay.log}"
log() { echo "[$(date '+%H:%M:%S.%3N')] touchbind: $*" >> "$LOG"; }

if ! command -v busctl >/dev/null 2>&1; then
  log "busctl not found (systemd required) — cannot bind touch"
  exit 3
fi

# busctl prints typed values: `s "libvirtualhid Touchscreen"`, `b true`, ...
# Strip the type prefix and the quotes.
prop() { busctl --user get-property org.kde.KWin "$1" org.kde.KWin.InputDevice "$2" 2>/dev/null || true; }
sval() { prop "$1" "$2" | sed -e 's/^[a-z]* //' -e 's/^"//' -e 's/"$//'; }

bound=0
touch_bound=0
i=0
while [ "$i" -lt 240 ]; do      # ~2 min: the client supplies the devices
  i=$((i + 1))
  sysnames=$(busctl --user get-property org.kde.KWin /org/kde/KWin/InputDevice \
               org.kde.KWin.InputDeviceManager devicesSysNames 2>/dev/null \
             | sed -e 's/^as //' -e 's/"//g')
  for sysname in $sysnames; do
    dev="/org/kde/KWin/InputDevice/$sysname"
    name=$(sval "$dev" name)
    case "$name" in *libvirtualhid*) ;; *) continue ;; esac
    istouch=$(sval "$dev" touch)
    [ "$istouch" = true ] || [ "$(sval "$dev" supportsCalibrationMatrix)" = true ] || continue
    was=$(sval "$dev" outputName)
    if [ "$was" != "$OUT" ]; then
      busctl --user set-property org.kde.KWin "$dev" org.kde.KWin.InputDevice \
        outputName s "$OUT" >/dev/null 2>&1 || true
      log "$sysname ($name) -> $OUT (was '$was')"
    fi
    [ "$(sval "$dev" outputName)" = "$OUT" ] || continue
    bound=1
    # Keep polling until the TOUCHSCREEN itself is bound: the absolute mouse and
    # pen devices show up first, and exiting early would leave touches dead.
    [ "$istouch" = true ] && touch_bound=1
  done
  [ "$touch_bound" = 1 ] && break
  sleep 0.5
done

[ "$bound" = 1 ] || log "gave up waiting for a libvirtualhid absolute-input device"
[ "$touch_bound" = 1 ] || log "no libvirtualhid touchscreen seen within 120s"
