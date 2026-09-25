#!/usr/bin/env bash
# Sunshine "virtual display" undo-hook — portable (no Nix) build.
#
# Restores what sunshine-vdisplay-do.sh changed:
#   1. the pre-session topology (snapshot taken by sunshine-topology.sh apply),
#   2. the touch binding waiter,
#   3. krfb-virtualmonitor (the virtual output disappears with it).
#
# Order matters: restoring the topology BEFORE killing krfb, because a session
# may have left the compositor with the virtual output as its only scanout.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="/usr/local/bin:/usr/bin:/bin:${HOME}/.local/bin:$PATH"

TOPOLOGY="${TOPOLOGY:-$HERE/sunshine-topology.sh}"
KSCREEN="${KSCREEN:-$(command -v kscreen-doctor || true)}"
VPORT="${SUNSHINE_VDISPLAY_PORT:-5910}"

LOG="${SUNSHINE_VDISPLAY_LOG:-${XDG_RUNTIME_DIR:-/tmp}/sunshine-vdisplay.log}"
export SUNSHINE_VDISPLAY_LOG="$LOG"
DRY="${SUNSHINE_VDISPLAY_DRYRUN:-}"

log() { echo "[$(date '+%H:%M:%S.%3N')] undo: $*" | tee -a "$LOG" >&2; }

if [ -n "$DRY" ]; then
  echo "would run: $TOPOLOGY revert"
  echo "would run: pkill -f sunshine-touchbind"
  echo "would run: pkill -f krfb-virtualmonitor.*$VPORT"
  exit 0
fi

[ -x "$TOPOLOGY" ] && "$TOPOLOGY" revert >> "$LOG" 2>&1 || true
pkill -f sunshine-touchbind 2>/dev/null || true
pkill -f "krfb-virtualmonitor.*$VPORT" 2>/dev/null || true
log "krfb killed (virtual output removed)"
exit 0
