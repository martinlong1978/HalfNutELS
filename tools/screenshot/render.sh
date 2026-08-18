#!/usr/bin/env bash
# Build the host renderer and write one PNG per scene into out/.
#
#   bash tools/screenshot/render.sh              # every scene
#   bash tools/screenshot/render.sh overlay-mode # just this one (repeatable)
#
# Each scene runs in its OWN process. lv_init() is global and Display gates it
# behind a per-object flag, so a second Display in one process would re-init
# LVGL under a live object tree; re-running the binary also guarantees no scene
# can look right merely because of state the previous one left behind.
#
# Every line of output is the verification record for that image: how many
# distinct colours it contains, what fraction of it differs from the modal
# (background) colour, and how many pixels LVGL never painted. `unpainted`
# must be 0 -- the panel has no transparent state, so anything else is a hole
# in the render and the run exits non-zero.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/out"
EXE="$HERE/build/elsshot.exe"

bash "$HERE/build.sh" || exit 1
mkdir -p "$OUT"

if [ $# -gt 0 ]; then
  SCENES=("$@")
else
  # tr -d '\r': the exe is a native Windows binary, so its stdout is in text
  # mode and every "\n" reaches us as "\r\n". Without this the scene names carry
  # a trailing CR and every lookup misses.
  mapfile -t SCENES < <("$EXE" --list | tr -d '\r')
fi

fail=0
for scene in "${SCENES[@]}"; do
  if ! "$EXE" "$scene" "$OUT"; then
    echo "  ^^ FAILED: $scene" >&2
    fail=1
  fi
done

echo
echo "[screenshot] ${#SCENES[@]} scene(s) -> $OUT"
exit "$fail"
