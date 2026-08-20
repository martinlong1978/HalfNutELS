#!/usr/bin/env python3
"""Generate the HalfNutELS brand mark in lib/display/icons/ as an LVGL v9 A8 image.

    halfNutLogo    96x96    the split threaded nut

DESIGN
------
The mark is the thing the project is named after: a lathe's HALF-NUT, drawn as a
hex nut parted across its centreline with the two halves stood clear of each
other, and a threaded bore through the middle.

Three things have to survive at 96 px on a panel read from a lathe's operating
position, so each is carried by silhouette rather than by detail:

    hex outline       -> "nut", read from the shape alone
    the parting gap   -> "HALF nut", a break in an otherwise solid body
    teeth in the bore -> "leadscrew", the one detail that says what it clamps

The gap is a straight horizontal break because that is where a real half-nut
splits, and the halves are pulled apart by SPLIT_SHIFT rather than merely
separated by a hairline: at this size a 1 px line reads as a rendering seam,
whereas open ground between two bodies cannot be mistaken for anything else.

Bore teeth are radial triangles spread evenly over the arc [ARC_LO, ARC_HI] of
each half, which keeps them clear of the parting line -- a tooth bisected by the
split reads as damage, not as thread. The two rows are mirrored rather than
staggered by half a pitch: a real thread in section does stagger, but at 96 px
that phase is invisible and the placement it forces walks the end teeth onto the
cut. Count and depth are tuned in the same direction (see TOOTH_COUNT): enough
crests to say "thread", few enough not to say "gear".

The mark is an alpha mask (A8), like the mode glyphs: the display recolours it
from the palette, so nothing here carries colour and the panel's R<->B swap does
not apply.

RASTERISATION
-------------
Union-of-adds minus union-of-subs, sampled at 8x8 subpixels and box downsampled,
giving real antialiased alpha. The nut is rasterised WHOLE and only then split,
in image space -- the halves are copied out of the finished buffer at a row
offset -- so both are guaranteed to be the same shape cut on the same line,
which two separately-clipped polygons cannot promise.

Pure standard library, and the shape primitives are shared with the mode glyphs
rather than re-derived. Re-run with `python scripts/make_logo.py`; add --ascii
to print a preview.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_glyphs import Polygon, merge          # noqa: E402  (shared primitives)

W, H = 96, 96
SS = 8                                # supersample factor per axis
CX, CY = W / 2.0, H / 2.0

# --- nut body ----------------------------------------------------------------
HEX_R = 41.0            # circumradius; vertices at 0 and 180 deg, so the top
                        # and bottom are FLAT edges -- a nut seen square-on.
BORE_R = 20.0

# --- the parting -------------------------------------------------------------
SPLIT_SHIFT = 3.0       # each half stands this far off the centreline, so the
                        # visible gap is twice it.

# --- bore teeth --------------------------------------------------------------
# Angles are measured from +x with y running DOWN the image, so a positive angle
# is below the centreline. The parting line therefore lies at 0 and 180 degrees,
# and both are kept clear: teeth are placed only on the arc [ARC_LO, ARC_HI] of
# each half, well inside the cut. A tooth bisected by the split reads as damage
# rather than as thread, which is exactly what an earlier revision of this file
# produced by placing them at fixed angles and letting the stagger walk two of
# them onto the parting line.
TOOTH_DEPTH = 4.5        # how far a crest stands into the bore
TOOTH_HALF_ANGLE = 13.0  # half the angular width of a tooth's base
TOOTH_COUNT = 3          # teeth per half. THREE, not four: at 96 px four per
                         # half read as eight small teeth around a ring -- a
                         # COG, which is precisely the wrong thing for a device
                         # whose whole purpose is to delete a lathe's change
                         # gears. Three deeper, wider crests per half read as a
                         # thread section instead.
ARC_LO, ARC_HI = 26.0, 154.0


class Circle(object):
    """Filled circle. Same spans(y) contract as the glyph script's shapes."""

    def __init__(self, cx, cy, r):
        self.cx, self.cy, self.r = cx, cy, r

    def spans(self, y):
        dy = y - self.cy
        if abs(dy) >= self.r:
            return []
        dx = math.sqrt(self.r * self.r - dy * dy)
        return [(self.cx - dx, self.cx + dx)]


def hexagon():
    """The nut body: vertices at 0, 60 ... 300 deg -> flat top and bottom."""
    return Polygon([(CX + HEX_R * math.cos(math.radians(a)),
                     CY + HEX_R * math.sin(math.radians(a)))
                    for a in range(0, 360, 60)])


def tooth(angle_deg):
    """One thread crest: a triangle on the bore, apex pointing at the centre."""
    a = math.radians(angle_deg)
    b0 = math.radians(angle_deg - TOOTH_HALF_ANGLE)
    b1 = math.radians(angle_deg + TOOTH_HALF_ANGLE)
    inner = BORE_R - TOOTH_DEPTH
    return Polygon([
        (CX + BORE_R * math.cos(b0), CY + BORE_R * math.sin(b0)),
        (CX + inner * math.cos(a), CY + inner * math.sin(a)),
        (CX + BORE_R * math.cos(b1), CY + BORE_R * math.sin(b1)),
    ])


def nut_shapes():
    """(body, bore, teeth) for the WHOLE, unsplit nut.

    Three lists and not two because the order matters: the teeth stand INTO the
    bore, so they have to be unioned on AFTER the bore has been cut out of the
    body. Union them with the body first and the bore subtraction simply eats
    them again, which is exactly what the first version of this did.
    """
    step = (ARC_HI - ARC_LO) / TOOTH_COUNT
    teeth = []
    for k in range(TOOTH_COUNT):
        a = ARC_LO + (k + 0.5) * step
        teeth.append(tooth(a))      # lower flank
        teeth.append(tooth(-a))     # upper flank, mirrored
    return [hexagon()], [Circle(CX, CY, BORE_R)], teeth


# -----------------------------------------------------------------------------
# Rasteriser: (union of adds) minus (union of subs), 8x8 box supersample -> A8
# -----------------------------------------------------------------------------

def subtract(adds, subs):
    """Span-list difference. Both lists must already be merged."""
    out = []
    for a, b in adds:
        cur = [(a, b)]
        for sa, sb in subs:
            nxt = []
            for x0, x1 in cur:
                if sb <= x0 or sa >= x1:
                    nxt.append((x0, x1))
                    continue
                if sa > x0:
                    nxt.append((x0, sa))
                if sb < x1:
                    nxt.append((sb, x1))
            cur = nxt
            if not cur:
                break
        out.extend(cur)
    return out


def rasterise(body, subs, extras):
    """Return a bytearray of W*H alpha values for (body - subs) + extras."""
    counts = [[0] * W for _ in range(H)]
    step = 1.0 / SS
    half = step / 2.0
    row = bytearray(W * SS)
    for sy in range(H * SS):
        y = sy * step + half
        spans = subtract(merge([s for sh in body for s in sh.spans(y)]),
                         merge([s for sh in subs for s in sh.spans(y)]))
        spans = merge(spans + [s for sh in extras for s in sh.spans(y)])
        if not spans:
            continue
        for i in range(len(row)):
            row[i] = 0
        for a, b in spans:
            k0 = int(math.ceil((a - half) * SS))
            k1 = int(math.floor((b - half) * SS))
            if k0 < 0:
                k0 = 0
            if k1 > W * SS - 1:
                k1 = W * SS - 1
            if k1 >= k0:
                row[k0:k1 + 1] = b"\x01" * (k1 - k0 + 1)
        crow = counts[sy // SS]
        for px in range(W):
            base = px * SS
            c = sum(row[base:base + SS])
            if c:
                crow[px] += c

    total = float(SS * SS)
    data = bytearray(W * H)
    i = 0
    for y in range(H):
        crow = counts[y]
        for x in range(W):
            data[i] = int(round(crow[x] / total * 255.0))
            i += 1
    return data


def split_halves(data):
    """Cut the finished nut on its centreline and stand the halves apart.

    Rows above the centreline shift UP by SPLIT_SHIFT, rows below shift DOWN.
    Working on the rasterised buffer rather than clipping the geometry twice
    guarantees both halves are cut on exactly the same line.
    """
    out = bytearray(W * H)
    mid = int(CY)
    shift = int(SPLIT_SHIFT)
    for y in range(H):
        dst = y - shift if y < mid else y + shift
        if dst < 0 or dst >= H:
            continue
        out[dst * W:(dst + 1) * W] = data[y * W:(y + 1) * W]
    return out


# -----------------------------------------------------------------------------
# LVGL v9 A8 .c emitter -- byte-for-byte the layout make_glyphs.py produces
# -----------------------------------------------------------------------------

HEADER = """#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef {attr}
#define {attr}
#endif

// Generated by scripts/make_logo.py -- do not hand-edit.
const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST {attr} uint8_t {map}[] = {{
"""

FOOTER = """}};

const lv_image_dsc_t {sym} = {{
  .header.cf = LV_COLOR_FORMAT_A8,
  .header.w = {w},
  .header.h = {h},
  .header.stride = {w},
  .data_size = {n},
  .data = {map},
}};
"""


def emit(path, sym, mapname, attr, data):
    lines = HEADER.format(attr=attr, map=mapname).split("\n")
    for i in range(0, len(data), 16):
        lines.append("  " + " ".join("0x%02x," % b for b in data[i:i + 16]))
    lines += FOOTER.format(sym=sym, map=mapname, w=W, h=H,
                           n=len(data)).split("\n")
    # lib/ sources are CRLF in this repo.
    with open(path, "wb") as f:
        f.write("\r\n".join(lines).encode("ascii"))


RAMPS = (" @", " .+#@", " .:-=+*#%@")


def ascii_art(data, ramp=RAMPS[-1], pair_rows=True):
    """Preview. Rows are paired so the aspect ratio survives a terminal cell."""
    step = 2 if pair_rows else 1
    out = []
    for y in range(0, H, step):
        line = []
        for x in range(W):
            v = data[y * W + x]
            if pair_rows and y + 1 < H:
                v = max(v, data[(y + 1) * W + x])
            line.append(ramp[min(len(ramp) - 1, v * len(ramp) // 256)])
        out.append("".join(line))
    return out


OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "lib", "display", "icons")


def main():
    body, subs, teeth = nut_shapes()
    data = split_halves(rasterise(body, subs, teeth))
    path = os.path.normpath(os.path.join(OUT_DIR, "halfNutLogo.c"))
    emit(path, "halfNutLogo", "halfNutLogo_map",
         "LV_ATTRIBUTE_IMG_HALF_NUT_LOGO", data)
    print("wrote %s (%d bytes of image data)" % (path, len(data)))
    if "--ascii" in sys.argv:
        for line in ascii_art(data):
            print(line)


if __name__ == "__main__":
    main()
