#!/usr/bin/env python3
"""Convert LVGL v9 I1 (1-bit indexed) image C files to A8 (8-bit alpha) format.

Why: LVGL's software renderer takes a fast, zero-allocation path when
recolouring an A8 image (it becomes a pure blend mask); every other format
falls back to an allocate-and-blend recolor_only() path on every draw. A8
lets the display recolour these icons for a runtime light/dark theme for
free. See lib/display/icons/ and CLAUDE.md feature set H/I.

Input format (as emitted by LVGL's image converter for LV_COLOR_FORMAT_I1):
  the `<name>_map[]` byte array starts with an 8-byte, 2-entry palette
  (4 bytes each, B,G,R,A order) for index 0 and index 1, followed by a
  1-bit-per-pixel bitmap, MSB-first within each byte, each row padded out
  to `.header.stride` bytes. In every icon in this repo index 0 is fully
  transparent and index 1 is fully opaque, so the conversion is a faithful
  bit -> byte expansion: set bit -> 0xFF, clear bit -> 0x00. Padding bits
  beyond `w` in each row are discarded (not just the trailing pad bytes).

Output: an A8 image where `.data` is exactly w*h bytes (one alpha byte per
pixel, stride == w, no palette) in the same LVGL v9 .c style, keeping the
file's original preamble (include guards, LV_ATTRIBUTE_* macros, any
leading comments above the array) unchanged, and the same descriptor
variable name and map array name.

Usage:
    python scripts/icons_i1_to_a8.py <file1.c> [file2.c ...]

Idempotent: running it again on an already-converted A8 file is a no-op
(the script detects LV_COLOR_FORMAT_A8 and leaves the file untouched).
"""

import re
import sys
from pathlib import Path

HEX_BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")

# Matches: `const <attrs> uint8_t <map_name>[] = {`  ... up to the matching `};`
MAP_ARRAY_RE = re.compile(
    r"(?P<preamble>.*?const\s+[\w\s]*?uint8_t\s+(?P<map_name>\w+)\s*\[\]\s*=\s*\{)"
    r"(?P<body>.*?)"
    r"(?P<close>\n\};\n)",
    re.DOTALL,
)

# Matches the trailing lv_image_dsc_t descriptor struct.
DESC_RE = re.compile(
    r"const\s+lv_image_dsc_t\s+(?P<symbol>\w+)\s*=\s*\{"
    r"(?P<body>.*?)"
    r"\};\n?",
    re.DOTALL,
)

CF_RE = re.compile(r"\.header\.cf\s*=\s*(\w+)")
W_RE = re.compile(r"\.header\.w\s*=\s*(\d+)")
H_RE = re.compile(r"\.header\.h\s*=\s*(\d+)")
STRIDE_RE = re.compile(r"\.header\.stride\s*=\s*(\d+)")
DATA_FIELD_RE = re.compile(r"\.data\s*=\s*(\w+)")


def format_byte_array(byte_values, bytes_per_line=16):
    lines = []
    for i in range(0, len(byte_values), bytes_per_line):
        chunk = byte_values[i : i + bytes_per_line]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def convert(text: str, path_for_errors: str) -> str:
    map_match = MAP_ARRAY_RE.search(text)
    if not map_match:
        raise ValueError(f"{path_for_errors}: could not locate map byte array")

    desc_match = DESC_RE.search(text)
    if not desc_match:
        raise ValueError(f"{path_for_errors}: could not locate lv_image_dsc_t descriptor")

    cf = CF_RE.search(desc_match.group("body"))
    if cf and cf.group(1) == "LV_COLOR_FORMAT_A8":
        return text  # already converted; idempotent no-op

    if not (cf and cf.group(1) == "LV_COLOR_FORMAT_I1"):
        raise ValueError(f"{path_for_errors}: unexpected .header.cf ({cf.group(1) if cf else '???'}), expected LV_COLOR_FORMAT_I1")

    def require_field(field_re, field_desc):
        m = field_re.search(desc_match.group("body"))
        if not m:
            raise ValueError(f"{path_for_errors}: could not locate {field_desc} in descriptor")
        return m.group(1)

    w = int(require_field(W_RE, ".header.w"))
    h = int(require_field(H_RE, ".header.h"))
    stride = int(require_field(STRIDE_RE, ".header.stride"))
    symbol = desc_match.group("symbol")
    map_name = map_match.group("map_name")
    data_field = require_field(DATA_FIELD_RE, ".data")
    if data_field != map_name:
        raise ValueError(
            f"{path_for_errors}: .data field ({data_field}) doesn't match map array name ({map_name})"
        )

    all_bytes = [int(b, 16) for b in HEX_BYTE_RE.findall(map_match.group("body"))]
    palette, bitmap = all_bytes[:8], all_bytes[8:]
    if len(palette) != 8:
        raise ValueError(f"{path_for_errors}: expected an 8-byte I1 palette, found {len(palette)} bytes")
    expected_bitmap_bytes = stride * h
    if len(bitmap) != expected_bitmap_bytes:
        raise ValueError(
            f"{path_for_errors}: bitmap byte count {len(bitmap)} != stride*h ({stride}*{h}={expected_bitmap_bytes})"
        )

    pixels = bytearray(w * h)
    for row in range(h):
        row_bytes = bitmap[row * stride : (row + 1) * stride]
        for col in range(w):
            byte_idx = col // 8
            bit_idx = 7 - (col % 8)  # MSB first
            bit = (row_bytes[byte_idx] >> bit_idx) & 1
            pixels[row * w + col] = 0xFF if bit else 0x00

    new_array_body = "\n\n" + format_byte_array(pixels) + "\n"
    new_array_block = map_match.group("preamble") + new_array_body + "};\n"

    data_size = w * h
    new_desc_block = (
        "const lv_image_dsc_t " + symbol + " = {\n"
        "  .header.cf = LV_COLOR_FORMAT_A8,\n"
        "  .header.w = " + str(w) + ",\n"
        "  .header.h = " + str(h) + ",\n"
        "  .header.stride = " + str(w) + ",\n"
        "  .data_size = " + str(data_size) + ",\n"
        "  .data = " + map_name + ",\n"
        "};\n"
    )

    result = text[: map_match.start()] + new_array_block
    result += text[map_match.end() : desc_match.start()]
    result += new_desc_block
    result += text[desc_match.end() :]
    return result


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    exit_code = 0
    for arg in argv[1:]:
        path = Path(arg)
        text = path.read_text()
        try:
            converted = convert(text, str(path))
        except ValueError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            exit_code = 1
            continue
        if converted == text:
            print(f"{path}: already A8, unchanged")
        else:
            path.write_text(converted)
            print(f"{path}: converted I1 -> A8")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
