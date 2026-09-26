#!/usr/bin/env bash
# Verify a built .deb before it is shipped.
#
#   verify-sunshine-deb.sh <path-to-deb>
#
# Three questions, in order:
#   1. does the package metadata say what we intended?
#   2. are the integration files the deb exists for actually inside?
#   3. do the binaries carry the marker strings of the fixes this build is supposed to ship?
#
# A version string is not evidence of a fix, so (3) greps the packaged binaries for the log
# lines that only exist in the patched code. Anything missing is reported as a FAILURE with
# the fix it corresponds to, so a package is never described as "has all the fixes".
set -uo pipefail

DEB="${1:?usage: verify-sunshine-deb.sh <path-to-deb>}"
EXPECT_VERSION="${EXPECT_VERSION:-2026.10.02}"
fail=0

say() { printf '%s\n' "$*"; }
bad() { printf '  !! %s\n' "$*"; fail=1; }

# --- 1. metadata ------------------------------------------------------------------------
say "== metadata"
dpkg-deb -f "$DEB" Package Version Architecture 2>&1 | sed 's/^/   /'
ver="$(dpkg-deb -f "$DEB" Version 2>/dev/null || echo '?')"
case "$ver" in
  *"$EXPECT_VERSION"*) say "   version matches $EXPECT_VERSION" ;;
  *) bad "version '$ver' does not contain $EXPECT_VERSION" ;;
esac
case "$(dpkg-deb -f "$DEB" Architecture 2>/dev/null || echo '?')" in
  amd64) say "   architecture amd64" ;;
  *) bad "architecture is not amd64" ;;
esac

# --- 2. integration files ---------------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
dpkg-deb -x "$DEB" "$TMP" || { bad "could not extract the package"; exit 1; }

say "== integration files"
find_one() {  # <description> <find-pattern>
  local found
  found="$(find "$TMP" -path "$2" 2>/dev/null | head -1)"
  if [ -n "$found" ]; then
    say "   ok   $1  (${found#"$TMP"})"
  else
    bad "missing $1  (looked for $2)"
  fi
}

find_one "the sunshine binary"        '*/bin/sunshine'
find_one "the systemd user unit"      '*systemd/user/*.service'
find_one "the udev rules"             '*udev/rules.d/*sunshine*.rules'
find_one "the modules-load file"      '*modules-load.d/*sunshine*.conf'
find_one "the application .desktop"   '*applications/*Sunshine.desktop'
find_one "the KWin permission entry"  '*applications/*Sunshine.kwin.desktop'
find_one "the web UI entry point"     '*/sunshine/web/index.html'

# postinst lives in the CONTROL archive, not the data tree - extract it explicitly.
CTRL="$(mktemp -d)"
if dpkg-deb -e "$DEB" "$CTRL" 2>/dev/null && [ -f "$CTRL/postinst" ]; then
  say "   ok   postinst present ($(wc -c < "$CTRL/postinst") bytes)"
  if grep -aqE 'setcap' "$CTRL/postinst"; then
    say "   ok   postinst applies capabilities (setcap)"
  else
    bad "postinst does not call setcap"
  fi
else
  bad "no postinst in the control archive (setcap / udev reload lives there)"
fi

# --- 3. the fixes this build is supposed to carry ---------------------------------------
say "== fix markers in the packaged binary"
bins="$(find "$TMP" -type f \( -name 'sunshine' -o -name '.sunshine-wrapped' -o -name 'sunshine-*' \) 2>/dev/null)"
if [ -z "$bins" ]; then
  bad "no sunshine binary found to grep"
fi

check_marker() {  # <marker string> <what it proves>
  local hits=0 f c
  for f in $bins; do
    # `grep -c` always prints a count but exits 1 when it is 0, so never append `|| echo 0`
    # here: that yields two values and the addition dies with a syntax error.
    c="$(grep -aFc -- "$1" "$f" 2>/dev/null)"
    hits=$(( hits + ${c:-0} ))
  done
  if [ "$hits" -gt 0 ]; then
    say "   ok   [$1] x$hits  <- $2"
  else
    bad "marker '$1' absent  <- $2"
  fi
}

check_marker "codec context moved to"        "runtime bitrate change on Linux (FFmpeg path)"
check_marker "handed to a running session"   "GET /bitrate accepted for a live session"
check_marker "after the compositor settled"  "topology two-pass re-enable (KWin neigbour fix)"
check_marker "topology: mode="               "built-in display-preparation modes"
check_marker "Virtual display ["             "built-in virtual display creation"
check_marker "kscreen listing"               "kscreen listing diagnostics (ANSI-strip fix path)"

say ""
if [ "$fail" -eq 0 ]; then
  say "VERDICT: OK — metadata, integration files and every expected fix marker are present."
else
  say "VERDICT: FAILED — see the '!!' lines above; do NOT ship this package as-is."
fi
say "size: $(du -h "$DEB" | cut -f1)   sha256: $(sha256sum "$DEB" | cut -d' ' -f1)"
exit "$fail"
