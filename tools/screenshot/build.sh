#!/usr/bin/env bash
# Build the host screen renderer. Called by render.sh; run directly only if you
# want to compile without rendering.
#
# Everything is compiled with gcc/g++ straight from the sources PlatformIO
# already downloaded for the device build, so the LVGL here is byte-for-byte the
# LVGL the device runs, configured by the project's real include/lv_conf.h.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build"
OBJ="$BUILD/obj"

# LVGL comes from the device env's PlatformIO libdeps -- the SAME checkout the
# firmware links against. Fall back to the publish env if only that one has been
# fetched.
LVGL=""
for env_dir in esp32dev_usb esp32dev_publish; do
  if [ -d "$ROOT/.pio/libdeps/$env_dir/lvgl/src" ]; then
    LVGL="$ROOT/.pio/libdeps/$env_dir/lvgl"
    break
  fi
done
if [ -z "$LVGL" ]; then
  echo "error: LVGL not found under $ROOT/.pio/libdeps/*/lvgl" >&2
  echo "       run 'pio pkg install -e esp32dev_usb' first." >&2
  exit 1
fi

CXX=${CXX:-g++}
CC=${CC:-gcc}
JOBS=${JOBS:-8}

# -DPIO_UNIT_TESTING=1 selects the SAME host doubles the unit tests use:
#   * lib/spindle/TestSpindle.cpp becomes the only Spindle:: definition
#     (ESPSpindle.cpp is hardware-only and is simply not in the file list);
#   * lib/global_state/globalstate.cpp drops its <Wire.h> include.
# TFT_WIDTH/TFT_HEIGHT mirror platformio.ini's device build flags, because
# lib/display derives DRAW_BUF_SIZE and the lv_tft_espi_create() geometry from
# them.
DEFS="-DPIO_UNIT_TESTING=1 -DLV_CONF_INCLUDE_SIMPLE -DTFT_WIDTH=240 -DTFT_HEIGHT=320"

# Shim dir FIRST so <Arduino.h> / <TFT_eSPI.h> / <SPI.h> resolve to the host
# versions; test/stubs after it for <ESP32Encoder.h>, which is reused as-is.
# test/test_leadscrew is on the path for leadscrewio_mock.h -- the unit tests'
# own LeadscrewIO double, reused rather than reinvented.
INCS="-I$HERE/shim -I$ROOT/test/stubs -I$ROOT/test/test_leadscrew \
 -I$ROOT/include -I$LVGL \
 -I$ROOT/lib/config -I$ROOT/lib/axis -I$ROOT/lib/spindle -I$ROOT/lib/leadscrew \
 -I$ROOT/lib/global_state -I$ROOT/lib/ui -I$ROOT/lib/dro -I$ROOT/lib/display \
 -I$ROOT/lib/ota"

CFLAGS="-O1 -w $DEFS $INCS"
CXXFLAGS="-O1 -w -std=gnu++17 $DEFS $INCS"

mkdir -p "$OBJ"

# --- source lists ------------------------------------------------------------
# All of LVGL's C sources. Every optional module is compiled out by the
# project's lv_conf.h, so the untouched ones cost only a preprocessor pass.
# LVGL's own .cpp files are all optional add-ons (ThorVG, glTF, LovyanGFX and
# the TFT_eSPI driver) and none is reachable with this config -- the TFT_eSPI
# one is REPLACED by shim/lv_tft_espi_host.cpp, which supplies the identical
# lv_tft_espi_create() symbol.
mapfile -t LV_SRCS < <(find "$LVGL/src" -name '*.c' | sort)

PROJ_SRCS=(
  "$ROOT/lib/config/latheconfig.cpp"
  "$ROOT/lib/global_state/globalstate.cpp"
  # GlobalState holds a DebugCapture by value and the Diagnostics screen
  # formats its status, so the renderer needs the real implementation - it is
  # pure C++ with no Arduino dependency, same as everything else here.
  "$ROOT/lib/global_state/debugcapture.cpp"
  "$ROOT/lib/spindle/TestSpindle.cpp"
  "$ROOT/lib/leadscrew/leadscrew.cpp"
  "$ROOT/lib/ui/uistate.cpp"
  "$ROOT/lib/dro/dro.cpp"
  # The OTA screen's wording is rendered by OtaOutcome, and the ota-* scenes
  # drive a real one rather than copying its strings (see scenes.cpp). Pure
  # C++, no ESP headers, exactly like lib/ui and lib/dro.
  "$ROOT/lib/ota/otaoutcome.cpp"
  "$ROOT/lib/display/ST7789_320_240displaylvgl.cpp"
  "$HERE/shim/lv_tft_espi_host.cpp"
  "$HERE/src/framebuffer.cpp"
  "$HERE/src/scenes.cpp"
  "$HERE/src/main.cpp"
)
# The three mode glyphs are generated C image descriptors (scripts/make_glyphs.py).
ICON_SRCS=("$ROOT"/lib/display/icons/*.c)

# --- compile -----------------------------------------------------------------
# Object names are the source path with separators flattened, so files of the
# same basename in different directories cannot collide.
objname() { echo "$OBJ/$(echo "${1#$ROOT/}" | tr '/\\:' '___').o"; }

compile_list() {
  local compiler="$1"; shift
  local flags="$1"; shift
  local -a jobs=()
  for src in "$@"; do
    local o
    o="$(objname "$src")"
    if [ -f "$o" ] && [ "$o" -nt "$src" ]; then
      continue
    fi
    jobs+=("$compiler $flags -c '$src' -o '$o'")
  done
  if [ ${#jobs[@]} -eq 0 ]; then
    return 0
  fi
  printf '%s\n' "${jobs[@]}" | xargs -P "$JOBS" -I{} bash -c '{}'
}

echo "[screenshot] compiling LVGL (${#LV_SRCS[@]} sources) ..."
compile_list "$CC" "$CFLAGS" "${LV_SRCS[@]}"

echo "[screenshot] compiling glyph assets ..."
compile_list "$CC" "$CFLAGS" "${ICON_SRCS[@]}"

echo "[screenshot] compiling display + renderer ..."
# The project/renderer sources are always rebuilt: they are the ones being
# iterated on, and there is no header dependency scanning here.
for src in "${PROJ_SRCS[@]}"; do
  rm -f "$(objname "$src")"
done
compile_list "$CXX" "$CXXFLAGS" "${PROJ_SRCS[@]}"

echo "[screenshot] linking ..."
# Via a response file, not argv: 459 LVGL objects is past Windows' 32 KB command
# line limit ("Argument list too long"). @file is understood by both gcc and
# clang and keeps the object list explicit, which an intermediate .a would not.
#
# The names inside it are BASENAMES and the link runs from $OBJ. On MSYS2 the
# runtime rewrites POSIX paths to Windows paths only in argv -- the contents of
# a response file are passed to ld verbatim, so "/c/..." inside one is a path
# the Windows linker has never heard of ("cannot find ..."). Basenames plus a
# working directory sidestep the whole question and keep the file portable.
RSP="$OBJ/link.rsp"
: > "$RSP"
for src in "${LV_SRCS[@]}" "${ICON_SRCS[@]}" "${PROJ_SRCS[@]}"; do
  basename "$(objname "$src")" >> "$RSP"
done
( cd "$OBJ" && "$CXX" "@link.rsp" -o "../elsshot.exe" )
echo "[screenshot] built $BUILD/elsshot.exe"
