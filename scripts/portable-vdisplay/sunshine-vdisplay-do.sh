#!/usr/bin/env bash
# Sunshine "virtual display" do-hook — portable (no Nix) build.
#
# Sunshine on Linux has no built-in virtual monitor: it only offers the
# client-visible ids.  Something on the host has to create the output, and this
# hook is that something:
#
#   * the client picked 虚拟-KWin / 虚拟-KMS -> krfb-virtualmonitor starts a KWin
#     virtual output at the client's resolution, this hook enables it, applies
#     the session's display-preparation mode and binds touch to it;
#   * the client picked a real monitor (or "默认") -> nothing to create, but the
#     requested output is made live if it was switched off, and touch is
#     (re)bound to it (a stale binding to a dead virtual output makes touches
#     land in the wrong place).
#
# Wire it up with (see README.md):
#   prep-cmd = ["do":["$HOME/.local/bin/sunshine-vdisplay-do.sh"],
#               "undo":["$HOME/.local/bin/sunshine-vdisplay-undo.sh"]]
#
# Requirements: Plasma Wayland session, kscreen-doctor, krfb (for the virtual
# output).  Test without touching anything:
#   SUNSHINE_VDISPLAY_DRYRUN=1 ./sunshine-vdisplay-do.sh

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="/usr/local/bin:/usr/bin:/bin:${HOME}/.local/bin:$PATH"

KSCREEN="${KSCREEN:-$(command -v kscreen-doctor || true)}"
VMONITOR="${VMONITOR:-$(command -v krfb-virtualmonitor || true)}"
TOUCHBIND="${TOUCHBIND:-$HERE/sunshine-touchbind.sh}"
TOPOLOGY="${TOPOLOGY:-$HERE/sunshine-topology.sh}"

LOG="${SUNSHINE_VDISPLAY_LOG:-${XDG_RUNTIME_DIR:-/tmp}/sunshine-vdisplay.log}"
export SUNSHINE_VDISPLAY_LOG="$LOG"
DRY="${SUNSHINE_VDISPLAY_DRYRUN:-}"
VPORT="${SUNSHINE_VDISPLAY_PORT:-5910}"
VNAME="${SUNSHINE_VDISPLAY_NAME:-SunshineVirt}"
VPASS="${SUNSHINE_VDISPLAY_PASSWORD:-sunshine-vm}"

log() { echo "[$(date '+%H:%M:%S.%3N')] do: $*" | tee -a "$LOG" >&2; }

log "start VIRTUAL='${SUNSHINE_CLIENT_VIRTUAL_DISPLAY:-<unset>}' ${SUNSHINE_CLIENT_WIDTH:-?}x${SUNSHINE_CLIENT_HEIGHT:-?}@${SUNSHINE_CLIENT_FPS:-?} disp='${SUNSHINE_CLIENT_DISPLAY_NAME:-<unset>}' dd_config='${SUNSHINE_CLIENT_DD_CONFIG:-<unset>}'"

IS_VIRTUAL=""
case "${SUNSHINE_CLIENT_VIRTUAL_DISPLAY:-}" in
  kwin|kms) IS_VIRTUAL=1 ;;
esac

# --- preflight ---------------------------------------------------------------
# A previous session's virtual output (or its topology) can outlive its undo
# hook.  When that happens the requested display is genuinely absent from the
# compositor's list, so Sunshine waits ~20 s for it while the client gives up
# after ~10 s.  Clean up before the lookup, never after it.
# (Dry-run keeps its hands off: nothing is reverted, nothing is killed.)
if [ -n "$DRY" ]; then
  echo "would run (preflight): $TOPOLOGY revert; pkill sunshine-touchbind${IS_VIRTUAL:+; pkill krfb-virtualmonitor}"
elif [ -x "$TOPOLOGY" ] && [ -x "$KSCREEN" ]; then
  "$TOPOLOGY" revert >> "$LOG" 2>&1 || true
fi
if [ -z "$DRY" ]; then
  pkill -f sunshine-touchbind 2>/dev/null || true
  if [ -z "$IS_VIRTUAL" ]; then
    pkill -f "krfb-virtualmonitor.*$VPORT" 2>/dev/null || true
    sleep 0.3
    log "preflight: leftover virtual display/topology cleaned up (non-virtual session)"
  fi
fi
export SUNSHINE_VDISPLAY_DRYRUN="$DRY"   # the helpers honour it too

bind_touch() {   # bind_touch <kwin-output-name>
  [ -x "$TOUCHBIND" ] || { log "touchbind helper missing at $TOUCHBIND"; return 0; }
  if [ -n "$DRY" ]; then
    echo "would start: $TOUCHBIND $1"
    return 0
  fi
  nohup "$TOUCHBIND" "$1" >/dev/null 2>&1 &
  log "touch binding handed to '$1'"
}

if [ -z "$IS_VIRTUAL" ]; then
  # --- real monitor / "默认" -------------------------------------------------
  TARGET="${SUNSHINE_CLIENT_DISPLAY_NAME:-}"
  if [ -z "$TARGET" ] && [ -x "$KSCREEN" ]; then
    TARGET=$("$KSCREEN" -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' \
      | awk '/^Output: / { name=$3 } /^[[:space:]]*enabled/ { if (name != "" && name !~ /^Virtual-/) { print name; exit } }')
  fi
  if [ -n "$TARGET" ]; then
    # The requested output may be switched off — then the compositor does not
    # enumerate it and the session cannot start.
    [ -x "$TOPOLOGY" ] && "$TOPOLOGY" ensure "$TARGET" >> "$LOG" 2>&1 || true
    bind_touch "$TARGET"
  else
    log "no enabled non-virtual output found — leaving display/touch untouched"
  fi
  exit 0
fi

# --- virtual monitor ---------------------------------------------------------
if ! command -v kwin_wayland >/dev/null 2>&1 && ! pgrep -u "$(id -u)" -f '/kwin_wayland' >/dev/null 2>&1; then
  log "no kwin_wayland session for uid $(id -u) — cannot create a virtual monitor"
  exit 0
fi
if [ -z "$VMONITOR" ]; then
  log "krfb-virtualmonitor not found — install krfb (KDE)"
  exit 1
fi
if [ "${SUNSHINE_CLIENT_VIRTUAL_DISPLAY}" = kms ]; then
  log "note: the KMS capture path needs CAP_SYS_ADMIN (an AppImage cannot get it); the output is created anyway, capture falls back to KWin"
fi

CW="${SUNSHINE_CLIENT_WIDTH:-1920}"
CH="${SUNSHINE_CLIENT_HEIGHT:-1080}"
VFPS="${SUNSHINE_CLIENT_FPS:-60}"

start_vmonitor() {
  WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
  DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus}" \
    "$VMONITOR" --resolution "${CW}x${CH}" --name "$VNAME" --port "$VPORT" --password "$VPASS" \
      >> "$LOG" 2>&1 &
}

if [ -n "$DRY" ]; then
  echo "would run: $VMONITOR --resolution ${CW}x${CH} --name $VNAME --port $VPORT (then enable + bind touch)"
  echo "would run: $TOPOLOGY apply Virtual-$VNAME"
  echo "would run: $TOUCHBIND Virtual-$VNAME"
  exit 0
fi

pkill -f "krfb-virtualmonitor.*$VPORT" 2>/dev/null || true
sleep 0.3
start_vmonitor

VOUT=""
for attempt in 1 2 3; do
  for _ in $(seq 1 20); do
    VOUT=$("$KSCREEN" -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' \
      | grep -oE "Virtual-$VNAME [0-9a-f-]+" | awk '{print $2}' | head -1)
    [ -n "$VOUT" ] && break
    sleep 0.3
  done
  [ -n "$VOUT" ] && break
  log "attempt $attempt: output not in kscreen, restarting krfb"
  pkill -f "krfb-virtualmonitor.*$VPORT" 2>/dev/null || true
  sleep 1
  start_vmonitor
done
if [ -z "$VOUT" ]; then
  log "giving up: Virtual-$VNAME never appeared (is krfb installed and is this a Plasma Wayland session?)"
  exit 1
fi

"$KSCREEN" "output.$VOUT.enable" >> "$LOG" 2>&1 || true
sleep 0.5
"$KSCREEN" "output.$VOUT.addCustomMode.$CW.$CH.${VFPS}000.full" >/dev/null 2>&1 || true
"$KSCREEN" "output.$VOUT.mode.${CW}x${CH}@$VFPS" >> "$LOG" 2>&1 || true
log "ready: $VOUT enabled at ${CW}x${CH}@${VFPS}"

# Display-preparation mode (the dd_* settings) — virtual output only, and
# before the touch binding: a topology change can drop KWin's per-device
# output binding.
[ -x "$TOPOLOGY" ] && "$TOPOLOGY" apply "Virtual-$VNAME" >> "$LOG" 2>&1 || true

bind_touch "Virtual-$VNAME"
exit 0
