#!/usr/bin/env bash
# Emit a per-file divergence + authorship table for the Linux-related surface:
#   upstream/master (LizardByte)  vs  HEAD (this fork, linux-support), both measured from BASE.
# Columns: oursC(commits) biaogo alkaid(+other) theirsC  ours_delta  theirs_delta  path
# Usage: scripts/gen-linux-divergence-table.sh [base] [ours] [theirs]
set -euo pipefail
BASE=${1:-$(git merge-base HEAD upstream/master)}
OURS=${2:-HEAD}
THEIRS=${3:-upstream/master}
PATHS=(
  'src/platform/linux'
  'src/platform/common.h'
  'src/platform/common.cpp'
  'src/platform/virtualhid_input.cpp'
  'src/platform/virtualhid_input.h'
  'cmake/compile_definitions/linux.cmake'
  'packaging/linux'
  'docker'
  'docs'
  'src/display_device.cpp'
  'src/display_device.h'
  'src/display_device'
  'src/system_tray.cpp'
  'src/system_tray.h'
  'src/input.cpp'
  'src/input.h'
)
# union of files changed on either side, restricted to the path set above
{ git diff --name-only "$BASE" "$OURS" -- "${PATHS[@]}";
  git diff --name-only "$BASE" "$THEIRS" -- "${PATHS[@]}"; } | sort -u > /tmp/fsl-an/linux-paths.txt
printf 'oursC\tbiaogo\tother\ttheirsC\tours_delta\ttheirs_delta\tpath\n'
while read -r f; do
  [ -z "$f" ] && continue
  oc=$(git log --no-merges --oneline "$BASE..$OURS" -- "$f" | wc -l)
  bc=$(git log --no-merges --author='Biaogo' --oneline "$BASE..$OURS" -- "$f" | wc -l)
  oc2=$((oc - bc))
  tc=$(git log --no-merges --oneline "$BASE..$THEIRS" -- "$f" | wc -l)
  od=$(git diff --numstat "$BASE" "$OURS" -- "$f" | awk -F'\t' '{print $1"+/"$2"-"}')
  td=$(git diff --numstat "$BASE" "$THEIRS" -- "$f" | awk -F'\t' '{print $1"+/"$2"-"}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$oc" "$bc" "$oc2" "$tc" "${od:--}" "${td:--}" "$f"
done < /tmp/fsl-an/linux-paths.txt
