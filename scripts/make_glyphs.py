#!/usr/bin/env python3
"""Generate the three mode glyphs in lib/display/icons/ as LVGL v9 A8 images.

The mode glyph is the only image left on the main screen (band 2, 128x64 at
188,47). drawMode() swaps one lv_image object between three sources:

    feedSymbol           FM_FEED           smooth shaft + arrow
    threadSymbol         FM_THREAD         right-hand thread, crests lean "\"
    threadSymbolReverse  FM_THREAD_REVERSE left-hand thread,  crests lean "/"

DESIGN
------
All three share one silhouette: a shaft on the machine axis running into a
blunt arrowhead, spanning the same x extent and centred on the same y. Only
the *texture* of the shaft changes, so the eye has nothing to do but read the
one informative feature:

    FEED     solid shaft   -> uninterrupted travel
    THREAD R shaft cut into crests leaning "\"
    THREAD L shaft cut into crests leaning "/"

R vs L is the safety-critical pair, so the lean is deliberately steep
(~31 degrees off vertical, ~62 degrees of separation) and the crests span the
full band height, making the difference structural rather than a fine detail.
The two are exact mirrors about the horizontal centreline, so they have
identical mass, extent and optical centre by construction.

Which lean is which is not arbitrary. For a right-hand helix advancing +x, the
front-facing (visible) crests run from top-left to bottom-right, i.e. "\".
That is also what the previous hand-drawn assets did, so the mapping is
unchanged.

The glyphs are alpha masks (A8): the display recolours them from the palette,
so nothing here carries colour and the panel's R<->B swap does not apply.

RASTERISATION
-------------
Each glyph is a union of convex shapes described analytically. The union is
sampled at 8x8 subpixels and box-downsampled to 128x64, which gives real
antialiased alpha (0..255, not 0/255) - the old assets came from a 1-bit
source and were hard-edged.

Pure standard library: no PIL, no SVG rasteriser, no external tools. Re-run
with `python scripts/make_glyphs.py`; add --ascii to print a preview.
"""

import math
import os
import sys

W, H = 128, 64
SS = 8                                # supersample factor per axis
CY = H / 2.0                          # every glyph is centred on this line

# --- shared armature ---------------------------------------------------------
BAND_H = 32.0                         # shaft / crest band height
BAND_T = CY - BAND_H / 2.0            # 16
BAND_B = CY + BAND_H / 2.0            # 48
SHAFT_X0 = 4.0
ARROW_BASE_X = 95.0                   # vertical back edge of the arrowhead
ARROW_TIP_X = 125.0
ARROW_H = 52.0                        # arrowhead height (1.6x the band)
RAIL_H = 5.0                          # root diameter: ties the crests together
SHAFT_OVERLAP = 4.0                   # shaft runs this far into the arrowhead

# --- thread crests -----------------------------------------------------------
# Four crests, not five or six: at 20 mm the gap between crests is the thing
# that has to survive, and four gives ~10 px (1.7 mm on the panel) of clear
# space between 11 px teeth. The lean is 19 px over the 32 px band -> ~31 deg
# off vertical, so R and L are ~62 deg apart. That separation is the whole
# point of the pair.
CREST_X0 = 4.0                        # crest band start (= shaft start)
CREST_X1 = 93.0                       # crest band end (runs into the arrow)
CREST_N = 4
CREST_T = 11.0                        # crest thickness, measured horizontally
CREST_LEAN = 19.0                     # x shift from band top to band bottom

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "lib", "display", "icons")


# -----------------------------------------------------------------------------
# Shapes. Each shape exposes spans(y) -> list of (x0, x1) covered at height y.
# -----------------------------------------------------------------------------

class RoundRect(object):
    """Axis-aligned rect with independent left/right corner radii."""

    def __init__(self, x0, y0, x1, y1, rl=0.0, rr=0.0):
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1
        self.rl, self.rr = rl, rr

    def _inset(self, y, r):
        if r <= 0.0:
            return 0.0
        if y < self.y0 + r:
            d = (self.y0 + r) - y
        elif y > self.y1 - r:
            d = y - (self.y1 - r)
        else:
            return 0.0
        if d >= r:
            return r
        return r - math.sqrt(r * r - d * d)

    def spans(self, y):
        if y < self.y0 or y > self.y1:
            return []
        a = self.x0 + self._inset(y, self.rl)
        b = self.x1 - self._inset(y, self.rr)
        return [(a, b)] if b > a else []


class Polygon(object):
    """Simple polygon, even-odd scanline fill."""

    def __init__(self, pts):
        self.pts = list(pts)

    def spans(self, y):
        xs = []
        pts = self.pts
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            if y0 == y1:
                continue
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        return [(xs[i], xs[i + 1]) for i in range(0, len(xs) - 1, 2)]


def clip_poly(pts, x0, y0, x1, y1):
    """Sutherland-Hodgman clip of a polygon against an axis-aligned rect."""
    def clip(poly, inside, intersect):
        out = []
        n = len(poly)
        for i in range(n):
            cur, prv = poly[i], poly[i - 1]
            ci, pi = inside(cur), inside(prv)
            if ci:
                if not pi:
                    out.append(intersect(prv, cur))
                out.append(cur)
            elif pi:
                out.append(intersect(prv, cur))
        return out

    def lerp(a, b, t):
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)

    poly = list(pts)
    poly = clip(poly, lambda p: p[0] >= x0,
                lambda a, b: lerp(a, b, (x0 - a[0]) / (b[0] - a[0])))
    if not poly:
        return poly
    poly = clip(poly, lambda p: p[0] <= x1,
                lambda a, b: lerp(a, b, (x1 - a[0]) / (b[0] - a[0])))
    if not poly:
        return poly
    poly = clip(poly, lambda p: p[1] >= y0,
                lambda a, b: lerp(a, b, (y0 - a[1]) / (b[1] - a[1])))
    if not poly:
        return poly
    poly = clip(poly, lambda p: p[1] <= y1,
                lambda a, b: lerp(a, b, (y1 - a[1]) / (b[1] - a[1])))
    return poly


# -----------------------------------------------------------------------------
# Glyph construction
# -----------------------------------------------------------------------------

def arrowhead():
    """The blunt travel arrow shared by all three glyphs."""
    return Polygon([(ARROW_BASE_X, CY - ARROW_H / 2.0),
                    (ARROW_TIP_X, CY),
                    (ARROW_BASE_X, CY + ARROW_H / 2.0)])


def rail():
    """The root diameter: ties the crests into one rod and reaches the arrow."""
    return RoundRect(SHAFT_X0, CY - RAIL_H / 2.0,
                     ARROW_BASE_X + SHAFT_OVERLAP, CY + RAIL_H / 2.0,
                     rl=RAIL_H / 2.0, rr=0.0)


def crests(lean):
    """CREST_N parallelograms across the band, clipped to the band rect.

    lean > 0 leans "\" (top-left to bottom-right) = right-hand thread.
    """
    shapes = []
    # Lay the crests out on the mid-height line so the pattern stays centred
    # whichever way it leans.
    span = CREST_X1 - CREST_X0
    pitch = span / CREST_N
    for i in range(CREST_N):
        xc = CREST_X0 + pitch * (i + 0.5)
        top = xc - lean / 2.0
        bot = xc + lean / 2.0
        quad = [(top - CREST_T / 2.0, BAND_T),
                (top + CREST_T / 2.0, BAND_T),
                (bot + CREST_T / 2.0, BAND_B),
                (bot - CREST_T / 2.0, BAND_B)]
        quad = clip_poly(quad, CREST_X0, BAND_T, CREST_X1, BAND_B)
        if len(quad) >= 3:
            shapes.append(Polygon(quad))
    return shapes


def glyph_feed():
    # A solid shaft, capped round at the tail: the smooth, uncut counterpart of
    # the threaded rod. Same envelope, same centre, no crests.
    shaft = RoundRect(SHAFT_X0, BAND_T,
                      ARROW_BASE_X + SHAFT_OVERLAP, BAND_B,
                      rl=BAND_H / 2.0, rr=0.0)
    return [shaft, arrowhead()]


def glyph_thread(lean):
    return [rail(), arrowhead()] + crests(lean)


# -----------------------------------------------------------------------------
# Rasteriser: union of shapes, 8x8 box supersample -> A8
# -----------------------------------------------------------------------------

def merge(spans):
    if not spans:
        return []
    spans = sorted(spans)
    out = [list(spans[0])]
    for a, b in spans[1:]:
        if a <= out[-1][1]:
            if b > out[-1][1]:
                out[-1][1] = b
        else:
            out.append([a, b])
    return out


def rasterise(shapes):
    """Return a bytearray of W*H alpha values."""
    counts = [[0] * W for _ in range(H)]
    step = 1.0 / SS
    half = step / 2.0
    row = bytearray(W * SS)
    for sy in range(H * SS):
        y = sy * step + half
        spans = merge([s for sh in shapes for s in sh.spans(y)])
        if not spans:
            continue
        for i in range(len(row)):
            row[i] = 0
        for a, b in spans:
            # subcolumn k covers x = k*step + half
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


# -----------------------------------------------------------------------------
# LVGL v9 A8 .c emitter - byte-for-byte the layout of the existing assets
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

// Generated by scripts/make_glyphs.py -- do not hand-edit.
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


# -----------------------------------------------------------------------------

RAMPS = (" @", " .+#@", " .:-=+*#%@")


def ascii_art(data, ramp=RAMPS[-1], pair_rows=True):
    """ASCII preview. Two pixel rows per text line (aspect-corrected), each
    text cell taking the max alpha of the pair, quantised onto `ramp`."""
    n = len(ramp)
    out = []
    step = 2 if pair_rows else 1
    for y in range(0, H, step):
        line = []
        for x in range(W):
            a = data[y * W + x]
            if pair_rows and y + 1 < H:
                a = max(a, data[(y + 1) * W + x])
            line.append(ramp[min(n - 1, a * n // 256)])
        out.append("".join(line))
    return out


GLYPHS = [
    ("feedSymbol.c", "feedSymbol", "feed_map",
     "LV_ATTRIBUTE_IMG_FEED", glyph_feed),
    ("threadSymbol.c", "threadSymbol", "threadSymbol_map",
     "LV_ATTRIBUTE_IMG_THREADSYMBOL", lambda: glyph_thread(+CREST_LEAN)),
    ("threadSymbolReverse.c", "threadSymbolReverse", "threadSymbolReverse_map",
     "LV_ATTRIBUTE_IMG_THREADSYMBOLREVERSE", lambda: glyph_thread(-CREST_LEAN)),
]


def main():
    show = "--ascii" in sys.argv
    for fname, sym, mapname, attr, build in GLYPHS:
        data = rasterise(build())
        path = os.path.normpath(os.path.join(OUT_DIR, fname))
        emit(path, sym, mapname, attr, data)
        print("wrote %s (%d bytes of image data)" % (path, len(data)))
        if show:
            for ramp in RAMPS:
                print("\n--- %s @ %d levels ---" % (sym, len(ramp)))
                for line in ascii_art(data, ramp):
                    print(line)


if __name__ == "__main__":
    main()
