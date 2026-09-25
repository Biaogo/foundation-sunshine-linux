#!/usr/bin/env bash
# Sunshine display-topology helper — portable (no Nix) build.
#
# Applies the session's requested display-preparation mode with kscreen-doctor
# and can revert it afterwards.  Used by sunshine-vdisplay-do.sh / -undo.sh.
#
#   SUNSHINE_CLIENT_DD_CONFIG=disabled|verify_only   -> touch nothing
#                            ensure_active           -> enable the target output
#                            ensure_primary          -> enable + priority 1
#                            ensure_only_display     -> enable the target, disable the others
#
# Usage: sunshine-topology.sh apply  <target-output-name>
#        sunshine-topology.sh ensure <output-name>
#        sunshine-topology.sh revert
#        sunshine-topology.sh list
#
# Add --dry-run (or set SUNSHINE_VDISPLAY_DRYRUN=1) to print the kscreen calls
# instead of running them.
#
# State: ${XDG_RUNTIME_DIR:-$HOME/.cache}/sunshine-topology.state — the layout
# seen at `apply` time, restored by `revert`.

set -u

KSCREEN="${KSCREEN:-$(command -v kscreen-doctor || true)}"
if [ -z "$KSCREEN" ]; then
  echo "sunshine-topology: kscreen-doctor not found (install the 'kscreen' package)" >&2
  exit 3
fi

LOG="${SUNSHINE_VDISPLAY_LOG:-${XDG_RUNTIME_DIR:-/tmp}/sunshine-vdisplay.log}"
STATE="${SUNSHINE_VDISPLAY_STATE:-${XDG_RUNTIME_DIR:-$HOME/.cache}/sunshine-topology.state}"
log() { echo "[$(date '+%H:%M:%S.%3N')] topology: $*" >> "$LOG"; }

ACTION="${1:?usage: sunshine-topology.sh apply|ensure|revert|list [target] [--dry-run]}"
TARGET="${2:-}"
DRY="${SUNSHINE_VDISPLAY_DRYRUN:-}"
[ "${3:-}" = "--dry-run" ] && DRY=1

# Output list as "uuid name enabled priority x y w h" lines.
outputs() {
  "$KSCREEN" -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '
    /^Output: / { if (name != "") print uuid, name, enabled, prio, geo; uuid=$4; name=$3; enabled="disabled"; prio=""; geo="" }
    /^[[:space:]]*enabled/  { enabled="enabled" }
    /^[[:space:]]*disabled/ { enabled="disabled" }
    /^[[:space:]]*priority/ { prio=$2 }
    /^[[:space:]]*Geometry:/ { geo=$2 " " $3 }
    /^[[:space:]]*$/ { if (name != "") print uuid, name, enabled, prio, geo; name="" }
    END  { if (name != "") print uuid, name, enabled, prio, geo }
  '
}

uuid_of() { outputs | awk -v n="$1" '$2 == n { print $1; exit }'; }

snapshot() {
  [ -n "$DRY" ] && return 0
  mkdir -p "$(dirname "$STATE")" 2>/dev/null || true
  outputs > "$STATE" 2>/dev/null || true
  log "snapshot written ($(wc -l < "$STATE" 2>/dev/null || echo 0) outputs)"
}

run() {
  if [ -n "$DRY" ]; then
    echo "would run: $KSCREEN $*"
  else
    log "run: $KSCREEN $*"
    "$KSCREEN" "$@" >> "$LOG" 2>&1 || true
  fi
}

case "$ACTION" in
  list)
    outputs | column -t 2>/dev/null || outputs
    ;;

  ensure)
    # Make a requested output usable.  A session that asks for a display which
    # is currently switched OFF can never start: the compositor does not
    # enumerate it, Sunshine waits for it and the client gives up.
    [ -n "$TARGET" ] || { echo "ensure needs an output name" >&2; exit 2; }
    UUID=$(uuid_of "$TARGET")
    if [ -z "$UUID" ]; then
      log "ensure: output '$TARGET' is not in kscreen — nothing to enable"
      exit 1
    fi
    snapshot
    run "output.$UUID.enable"
    if [ -n "$DRY" ]; then
      log "ensure: $TARGET ($UUID) — dry-run: nothing enabled"
    else
      log "ensure: $TARGET ($UUID) enabled"
    fi
    ;;

  apply)
    TARGET="${TARGET:-Virtual-SunshineVirt}"
    MODE="${SUNSHINE_CLIENT_DD_CONFIG:-}"
    [ -z "$MODE" ] && MODE=disabled

    # Always snapshot: `revert` is also what puts a physical output back when an
    # earlier session (or a lagging teardown) left the compositor with only the
    # virtual output.
    snapshot

    case "$MODE" in
      disabled|verify_only)
        log "mode=$MODE — leaving the topology untouched"
        exit 0
        ;;
      ensure_active|ensure_primary|ensure_only_display) ;;
      *)
        log "mode=$MODE is not a known topology mode — leaving the topology untouched"
        exit 0
        ;;
    esac

    UUID=$(uuid_of "$TARGET")
    if [ -z "$UUID" ]; then
      log "mode=$MODE but output '$TARGET' is not in kscreen — nothing to apply"
      exit 1
    fi

    run "output.$UUID.enable"
    [ "$MODE" = ensure_primary ] && run "output.$UUID.priority.1"
    if [ "$MODE" = ensure_only_display ]; then
      # Enable the target first so the compositor never loses its only scanout.
      outputs | awk -v keep="$TARGET" '$2 != keep { print $1 }' | while read -r other; do
        run "output.$other.disable"
      done
    fi
    if [ -n "$DRY" ]; then
      echo "(dry-run: nothing applied)"
      log "mode=$MODE — dry-run: nothing applied"
    else
      log "mode=$MODE applied to $TARGET ($UUID)"
    fi
    ;;

  revert)
    if [ ! -s "$STATE" ]; then
      log "revert: no snapshot — nothing to restore"
      exit 0
    fi
    while read -r uuid name enabled prio geo; do
      [ -z "${name:-}" ] && continue
      # The virtual output dies with krfb — never try to re-enable it.
      case "$name" in Virtual-*) continue ;; esac
      if [ "$enabled" = "enabled" ]; then run "output.$uuid.enable"; else run "output.$uuid.disable"; fi
      [ -n "$prio" ] && run "output.$uuid.priority.$prio"
      [ -n "$geo" ] && run "output.$uuid.position.$geo"
    done < "$STATE"
    [ -n "$DRY" ] || rm -f "$STATE"
    log "revert: pre-session topology restored"
    ;;

  *)
    echo "usage: sunshine-topology.sh apply|ensure|revert|list [target] [--dry-run]" >&2
    exit 2
    ;;
esac
