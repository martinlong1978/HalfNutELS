#include <config.h>

#if ELS_DISPLAY == ST7789_240_135_LVGL
#include <ST7789_320_240displaylvgl.h>
#include <globalstate.h>
#include <dro.h>
#include <version.h>  // FIRMWARE_VERSION, shown on the About screen

#if !PIO_UNIT_TESTING
// The About screen reads the station IP straight from WiFi (there is no other
// runtime holder of it). Device build only: host builds (tests / screenshot
// renderer) have no WiFi and inject the address via hostSetAboutNetwork()
// instead -- same guard convention as leadscrew.h.
#include <WiFi.h>
#endif

#include <stdio.h>
#include <string.h>
#include <math.h>  // fabsf, for the Diagnostics EXPECT column

// --- Runtime colour palette --------------------------------------------------
//
// docs/ux-redesign.md section 8 "Theme": dark and light are both legitimate
// looks on this panel depending on ambient light, so the palette is a runtime
// choice driven by LatheConfig::theme (THEME_DARK/THEME_LIGHT, latheconfig.h),
// not a compile-time one. Both instances below are compiled in (a few dozen
// bytes); Display::m_palette (set in the constructor, re-pointed only by
// setTheme()) selects the active one. This replaces the five COLOUR_* #defines
// that used to live here.
//
// *** COLOUR-ORDER TRAP -- READ BEFORE EDITING ANY VALUE BELOW ***
// This panel is wired R<->B swapped, so every lv_color_hex() value in BOTH
// palettes must be authored PRE-SWAPPED to compensate (CLAUDE.md "Display";
// this is exactly what the removed COLOUR_* constants used to do -- e.g. what
// reads as COLOUR_RED = lv_color_hex(0x0000FF) renders as red on this panel).
// NEVER paste a natural/designer RGB hex straight in here -- swap its R and B
// bytes first (0xRRGGBB -> 0xBBGGRR) and show the arithmetic in a comment, as
// done for the light palette's state colours below. A value with R==B (greys,
// black, white) is its own swap and needs no adjustment -- say so rather than
// leaving it looking unswapped-by-omission.
struct DisplayPalette {
  lv_color_t background;     // screen ground, painted onto the active screen by
                              // Display::init().
  lv_color_t textPrimary;    // high-emphasis ink: the mode-symbol icon, the
                              // pitch and RPM values, the carriage marker.
  lv_color_t textDim;        // low-emphasis ink: units, the non-datum end of the
                              // travel bar, the soft-key hints.
  lv_color_t colourRun;      // armed / SET / synced state.
  lv_color_t colourCaution;  // jogging / returning state.
  lv_color_t colourFault;    // halted, and reverse-spindle RPM text.
  lv_color_t colourDisabled; // inactive state, band rules, unfilled tracks.
  lv_color_t iconInk;        // recolour for the 32x32 status icons. The main
                              // screen no longer draws any of them (the chips
                              // they sat on are gone -- see the header); kept
                              // for the FS-I3 overlays/menu.
  lv_color_t surface;        // ground of the selector overlay panel. MUST differ
                              // from `background`: the panel is opaque (no
                              // translucency is affordable, section 8) and its
                              // only other edge cue is a 2px border, so a panel
                              // painted the same colour as the screen would read
                              // as an outline floating over live content rather
                              // than as a thing that has replaced it.
  lv_color_t accent;         // focus accent: the overlay border and the selected
                              // MODE tile. The one colour that means "the arrows
                              // drive THIS" (section 1).
  lv_color_t chipInk;        // ink used ON TOP of a filled chip (accent, the
                              // state colours, colourDisabled). Always a DARK
                              // ink in both palettes: the vivid fills are all
                              // mid-luminance, so a light ink fails contrast on
                              // every one of them (white on colourRun is 2.2:1)
                              // while a near-black ink clears 3.9:1 on the
                              // dimmest (colourDisabled) and ~9:1 on the rest.
                              // This is what lets the state word, the SYNC chip
                              // and the selected tiles stay legible in BOTH
                              // themes -- coloured TEXT on a light ground is
                              // unreadable (colourRun on white is 2.2:1), so
                              // anything state-coloured is a filled chip with
                              // this ink instead.
};

// Shared state / accent hues, from docs/ux-redesign.md section 8 "Colour":
// "the accent/state hues are shared so only the ground and text tokens differ",
// so the vivid set appears ONCE here and both palettes reference it. The doc
// gives them pre-swapped already; the swap arithmetic (R<->B byte swap of the
// doc's natural hex) is reproduced so it's checkable without cross-referencing:
//   run:      natural #00C853 -> R=00,G=C8,B=53 -> swap R/B -> 53,C8,00 -> 0x53C800
//   caution:  natural #FFAB00 -> R=FF,G=AB,B=00 -> swap R/B -> 00,AB,FF -> 0x00ABFF
//   fault:    natural #FF3B30 -> R=FF,G=3B,B=30 -> swap R/B -> 30,3B,FF -> 0x303BFF
//   disabled: natural #6B7280 -> R=6B,G=72,B=80 -> swap R/B -> 80,72,6B -> 0x80726B
//   accent:   natural #38BDF8 -> R=38,G=BD,B=F8 -> swap R/B -> F8,BD,38 -> 0xF8BD38
#define STATE_RUN       lv_color_hex(0x53C800)
#define STATE_CAUTION   lv_color_hex(0x00ABFF)
#define STATE_FAULT     lv_color_hex(0x303BFF)
#define STATE_DISABLED  lv_color_hex(0x80726B)
#define FOCUS_ACCENT    lv_color_hex(0xF8BD38)

// Dark palette (THEME_DARK, the default): a genuinely dark ground -- near-black
// blue-grey with near-white ink. (Its previous values were the pre-redesign
// light-grey screen, kept during the layout pass; that "grey and unreadable"
// look is exactly what this replaces.)
static const DisplayPalette PALETTE_DARK = {
  lv_color_hex(0x14110E), // background: natural #0E1114 -> R=0E,G=11,B=14 ->
                          // swap R/B -> 14,11,0E -> 0x14110E. Painted onto the
                          // screen explicitly by init() -- relying on LVGL's
                          // default (light grey) ground is why "dark" used to
                          // render light.
  lv_color_hex(0xF7F5F2), // textPrimary: natural #F2F5F7 -> R=F2,G=F5,B=F7 ->
                          // swap R/B -> F7,F5,F2 -> 0xF7F5F2. 17.3:1 on the
                          // background.
  lv_color_hex(0x94877C), // textDim: natural #7C8794 -> R=7C,G=87,B=94 ->
                          // swap R/B -> 94,87,7C -> 0x94877C. 5.2:1 on the
                          // background, 4.6:1 on the surface.
  STATE_RUN,              // colourRun (shared, swap working above).
  STATE_CAUTION,          // colourCaution (shared, swap working above).
  STATE_FAULT,            // colourFault (shared, swap working above).
  STATE_DISABLED,         // colourDisabled (shared, swap working above).
  lv_color_hex(0xFFFFFF), // iconInk: white -- R==B, its own swap.
  lv_color_hex(0x241E19), // surface: natural #191E24 -> R=19,G=1E,B=24 ->
                          // swap R/B -> 24,1E,19 -> 0x241E19. One step up from
                          // the background, so the opaque overlay panel reads
                          // as a thing that replaced the screen, not an outline.
  FOCUS_ACCENT,           // accent (shared, swap working above).
  lv_color_hex(0x14110E), // chipInk: the background colour again -- text is
                          // "punched out" of a filled chip down to the ground.
                          // Same value as background above, same swap working.
};

// Light palette. Same vivid state/accent hues; only the ground and ink tokens
// differ (docs/ux-redesign.md section 8 "Theme").
static const DisplayPalette PALETTE_LIGHT = {
  lv_color_hex(0xFFFFFF), // background: white -- R==B, its own swap.
  lv_color_hex(0x110E0B), // textPrimary: natural #0B0E11 -> R=0B,G=0E,B=11 ->
                          // swap R/B -> 11,0E,0B -> 0x110E0B. 19.4:1 on white.
  lv_color_hex(0x7C7268), // textDim: natural #68727C -> R=68,G=72,B=7C ->
                          // swap R/B -> 7C,72,68 -> 0x7C7268. 4.9:1 on white,
                          // 4.2:1 on the surface.
  STATE_RUN,              // colourRun (shared, swap working above).
  STATE_CAUTION,          // colourCaution (shared, swap working above).
  STATE_FAULT,            // colourFault (shared, swap working above).
  STATE_DISABLED,         // colourDisabled (shared, swap working above).
  // iconInk: BLACK here, unlike dark's white -- R==B, its own swap.
  //
  // The status icons are recoloured to iconInk and (in the overlays that will
  // use them) drawn ON TOP of the state colours above, so this pairing is what
  // has to be legible, not the ink against the screen background. White on the
  // vivid hues is washed out -- measured contrast is 2.2:1 on colourRun and
  // 1.9:1 on colourCaution, both under the 3:1 minimum for UI glyphs. Black
  // against the same two is roughly 9.4:1 and 11:1. (This is the same class of
  // mistake the I1->A8 icon conversion already made once -- black ink landing
  // on the green "engaged" chip -- so it is spelled out rather than left to be
  // rediscovered. It is also exactly why chipInk exists and is dark in BOTH
  // palettes.)
  lv_color_hex(0x000000),
  lv_color_hex(0xF0EEEB), // surface: natural #EBEEF0 -> R=EB,G=EE,B=F0 ->
                          // swap R/B -> F0,EE,EB -> 0xF0EEEB. One step DOWN
                          // from this palette's white ground -- the opposite
                          // direction to dark's, because there is nowhere
                          // above white to go.
  FOCUS_ACCENT,           // accent (shared, swap working above).
  lv_color_hex(0x110E0B), // chipInk: this palette's textPrimary again (natural
                          // #0B0E11, same swap working as above).
};

// --- Layout ------------------------------------------------------------------
//
// docs/ux-redesign.md section 8, "Layout -- 320x240 landscape". Five horizontal
// bands separated by 1px rules at BAND_*_BOTTOM:
//
//     0 +------------------------------------------+
//       | THREAD R   mm   SYNC            1250 RPM |  status bar
//    34 +------------------------------------------+
//       |  1.25 mm                        [glyph]  |  primary readout
//   122 +------------------------------------------+
//       |  . . . . I . . . . . . . .               |  pitch pips (discrete)
//   150 +------------------------------------------+
//       | |--o---------------|                       |
//       | L 0.00        12.40 mm             48.00 R |  carriage travel
//   205 +------------------------------------------+
//       |  [ CUTTING ]                             |  state chip (26)
//   240 +------------------------------------------+
//
// The soft-key hint row that band 5 used to carry is GONE: the physical caps
// are properly labelled, so mirroring them wasted the band. Band 3 is a row of
// discrete PIPS, not a slider: the pitch list is a discrete set of ~20 values,
// and a continuous track both misrepresented the control and read as a twin of
// the band-4 travel bar directly below it. The state word is 26 (owner: 36 was
// slightly too big), and the height that freed went into bands 3/4 as air so
// the pip row and the travel bar read as two instruments, not one stack.
//
// Every y below is derived from those boundaries plus the ACTUAL Montserrat
// metrics (line_height / base_line from lvgl's lv_font_montserrat_*.c):
//     size 14: line_height 16, base_line 3  -> ascent 13
//     size 26: line_height 29, base_line 5  -> ascent 24
//     size 36: line_height 40, base_line 7  -> ascent 33
//     size 48: line_height 52, base_line 9  -> ascent 43
// A single-line label's height IS its font's line_height, so "baseline-align a
// 14 with a 26" means y14 = y26 + (24 - 13) = y26 + 11.
//
// Only Montserrat 14/26/36/48 are compiled in (include/lv_conf.h) -- any other
// size fails to link.
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;

static const int BAND_STATUS_BOTTOM = 34;
static const int BAND_PITCH_BOTTOM = 122;
static const int BAND_TICKER_BOTTOM = 150;
static const int BAND_TRAVEL_BOTTOM = 205;

// Band 1 -- status bar, 0..33 (h 34).
// 26 centred: (34-29)/2 = 2. 14 centred: (34-16)/2 = 9.
// "RPM" is baseline-aligned to the value instead: 2 + 11 = 13.
static const int STATUS_CHIP_Y = 9;
static const int STATUS_MODE_X = 8;    // widest text "THREAD R" = 75px -> 83
static const int STATUS_UNIT_X = 96;   // widest text "inch" = 31px -> 127
static const int STATUS_SYNC_X = 140;  // "SYNC" ink 140..179; the label carries
                                       // a chip fill when synced, so its BOX is
                                       // padded SYNC_CHIP_PAD_H beyond the ink
                                       // on each side (134..185) -- the two
                                       // adjacency asserts below are written
                                       // against the padded box, not the ink.
static const int SYNC_CHIP_PAD_H = 6;
static const int SYNC_CHIP_PAD_V = 3;  // chip 6..27 vertically, ink still at 9
static const int STATUS_RPM_VALUE_Y = 2;
static const int STATUS_RPM_VALUE_X = 186;  // right-aligned box, 186..266;
static const int STATUS_RPM_VALUE_W = 80;   // "9999" @26 = 64px -> ink from 202
static const int STATUS_RPM_UNIT_X = 272;   // "RPM" @14 = 34px -> 306
static const int STATUS_RPM_UNIT_Y = 13;

// Band 2 -- primary readout, 35..121 (h 87).
// 48 centred: 35 + (87-52)/2 = 52. Glyph centred: 35 + (87-64)/2 = 46.
static const int PITCH_VALUE_X = 10;
static const int PITCH_VALUE_Y = 52;
static const int PITCH_UNIT_GAP = 8;
// lv_obj_align_to(OUT_RIGHT_BOTTOM) bottom-aligns the two boxes: that puts the
// 26 at y = 53 + (52-29) = 76, baseline 100, against the 48's baseline 96. The
// -4 offset pulls it back onto the same baseline.
static const int PITCH_UNIT_BASELINE_FIX = -4;
static const int MODE_GLYPH_X = 188;  // 128 wide -> 188..315
static const int MODE_GLYPH_Y = 46;   // 64 tall  -> 46..109

// Band 3 -- the pitch pip row, 123..149 (h 27). One pip per entry of the
// current pitch list, all bottom-aligned on a common baseline like a ruler:
// the current entry is the tall accent pip, its immediate neighbours are
// mid-height textDim, the rest are short colourDisabled ticks. Discrete pips,
// not a slider: the list is a discrete set, and the old continuous track read
// as a second travel bar. The row's usable width matches the travel track so
// the two bands stay column-aligned.
// Baseline at 144: tall pip 128..144 (5px clear of each band edge).
static const int PIP_ROW_X = 12;
static const int PIP_ROW_W = 296;      // 12..307, same as the travel track
static const int PIP_BASE_Y = 144;     // common bottom edge of every pip
static const int PIP_W = 4;
static const int PIP_H_CURRENT = 16;   // accent
static const int PIP_H_NEIGHBOUR = 10; // textDim
static const int PIP_H_OTHER = 6;      // colourDisabled

// Band 4 -- carriage travel, 151..204 (h 54). Grew again when the state chip
// dropped 36 -> 26: the extra height is AIR between the pip baseline (144) and
// the stop markers (156), so the pip row and the travel bar stop reading as
// one crowded control stack.
static const int TRAVEL_TRACK_X = 12;
static const int TRAVEL_TRACK_Y = 159;
static const int TRAVEL_TRACK_W = 296;  // 12..307
static const int TRAVEL_TRACK_H = 10;
static const int TRAVEL_MARK_Y = 156;   // 16 tall -> 156..171, clear of the rule
static const int TRAVEL_MARK_H = 16;
static const int TRAVEL_MARK_W = 4;
static const int TRAVEL_CARRIAGE_W = 8;
// 26 at y 173 -> box 173..201, baseline 197. The 14s sit at 197-13 = 184.
static const int TRAVEL_VALUE_Y = 173;
static const int TRAVEL_LABEL_Y = 184;
static const int TRAVEL_POS_X = 90;    // right-aligned box 90..200
static const int TRAVEL_POS_W = 110;   // "-300.00" @26 = 100px -> ink from 100
static const int TRAVEL_POS_UNIT_X = 204;  // "mm" @14 = 30px -> 234
static const int TRAVEL_LEFT_X = 12;       // "L 888.88" @14 = 60px -> 72
static const int TRAVEL_LEFT_W = 74;       // 12..85; see fixLabelBox() at its
                                           // creation. Worst realistic string is
                                           // "L -1200.00" @14 = 69px; beyond that
                                           // it CLIPS rather than growing into
                                           // travelPosLabel's box at x 90.
static const int TRAVEL_RIGHT_X = 238;     // right-aligned box 238..308
static const int TRAVEL_RIGHT_W = 70;      // "888.88 R" @14 = 62px -> ink from 246

// Band 5 -- the state chip, 206..239 (h 34). One Montserrat-26 label carrying
// its own state-coloured background (chip), chipInk text. 26, not 36: the
// owner judged 36 slightly too big, and the freed height went to bands 3/4.
// 26 line_height 29 -> chip 208..236, centred in the band.
static const int STATE_CHIP_X = 10;
static const int STATE_CHIP_Y = 208;
static const int STATE_CHIP_PAD_H = 12;  // horizontal chip padding around the ink

// Font box heights, for the layout assertions below (see the metrics table
// above): a single-line label is exactly line_height tall.
static const int FONT14_H = 16;
static const int FONT26_H = 29;
static const int FONT36_H = 40;
static const int FONT48_H = 52;
static const int GLYPH_W = 128;
static const int GLYPH_H = 64;
// Ascents (line_height - base_line), from the same metrics table. Needed to
// baseline-align labels of different sizes on one row: a label's box top is
// `baseline - ascent`, so aligning a 26 to a 48 is y26 = y48 + (43 - 24).
static const int FONT26_ASCENT = 24;
static const int FONT48_ASCENT = 43;

// Measured advance widths of the worst-case string in each fixed slot, summed
// from the adv_w fields of the same lv_font_montserrat_*.c files the metrics
// above come from (adv_w is 8.4 fixed point; per-glyph px = (adv_w + 8) >> 4,
// matching lv_font_fmt_txt_get_glyph_dsc). These exist so the horizontal
// adjacencies below are checked against real ink extents rather than against
// each other -- an assertion that only compares two x constants proves nothing
// about whether the text between them fits.
static const int TEXT14_MODE_W = 74;       // "THREAD R" (longest mode word)
static const int TEXT14_UNIT_W = 32;       // "inch"
static const int TEXT14_SYNC_W = 39;       // "SYNC"
static const int TEXT14_RPM_UNIT_W = 33;   // "RPM"
static const int TEXT26_RPM_VALUE_W = 64;  // "9999"
static const int TEXT26_STATE_W = 161;     // "RETURNING" (longest state word, at
                                           // the chip's new size 26)

// --- Selector overlay --------------------------------------------------------
//
// docs/ux-redesign.md section 4: press MODE / RATE / STOPS (or OK at rest for
// jog speed) and a solid panel replaces the CENTRE of the screen; the arrows
// adjust, OK or 4 s of idle dismisses it. Four focuses, one panel: the status
// bar (band 1) and the state bar (band 5) stay visible underneath so the
// machine never disappears while a setting is being changed.
//
//    40 +--- 2px accent border ----------------------+
//       |                  PITCH                     |  title      (14, dim)
//       |    1.00        1.25        1.50            |  ticker  (26/48/26)
//       |                  mm                        |  unit       (26, dim)
//       |  <> pitch                        OK done   |  hints      (14, dim)
//   196 +--------------------------------------------+
//
// The panel is 8..311 x 40..204: its bottom edge lands exactly on the band-4/5
// rule, so it covers band 4 COMPLETELY. Anything less leaves a strip of the
// travel readout's half-clipped glyphs visible under the panel edge, which
// reads as a rendering fault (it did, before the panel grew to meet the rule).
// Child coordinates below are relative to its CONTENT area, which LVGL insets
// by the border width (lv_obj_get_style_space_* adds border_width to the
// padding), so the usable box is 300 x 161 and every child x/y is measured
// from (OVERLAY_X + OVERLAY_BORDER, OVERLAY_Y + OVERLAY_BORDER). Groups sit at
// (0,0) at full content size and pass their children's coordinates through
// unchanged.
static const int OVERLAY_X = 8;
static const int OVERLAY_Y = 40;
static const int OVERLAY_W = 304;   // 8..311
static const int OVERLAY_H = 165;   // 40..204
static const int OVERLAY_BORDER = 2;
static const int OVERLAY_CONTENT_W = OVERLAY_W - (2 * OVERLAY_BORDER);  // 300
static const int OVERLAY_CONTENT_H = OVERLAY_H - (2 * OVERLAY_BORDER);  // 144

// Rows shared by all four focuses: a title at the top, a hint row at the
// bottom, and the body between them.
static const int OVERLAY_TITLE_Y = 5;    // 14 -> 5..20
static const int OVERLAY_BODY_TOP = 24;
static const int OVERLAY_BODY_BOTTOM = 135;
static const int OVERLAY_HINT_Y = 139;   // 14 ink -> 139..154
// The left hint doubles as a REFUSAL chip ("moving - stops locked"): when the
// hint variant is a block it takes a colourCaution fill with chipInk text --
// coloured TEXT would be unreadable on the light surface (caution on #EBEEF0
// is 1.5:1), a filled chip is ~10:1 in both palettes. The label is auto-width
// (the chip must hug its text) and permanently padded, positioned so the INK
// stays at (OVERLAY_HINT_L_X, OVERLAY_HINT_Y) whether or not the fill shows.
static const int OVERLAY_HINT_L_X = 6;
static const int OVERLAY_HINT_PAD_H = 6;
static const int OVERLAY_HINT_PAD_V = 2;  // chip 120..141, inside the 144 content
static const int OVERLAY_HINT_R_X = 200;
static const int OVERLAY_HINT_R_W = 94;   // 200..293

// Ticker (RATE and JOG SPEED): value at 48 in the centre, one neighbour either
// side at 26, both dimmed. Three fixed boxes rather than a layout manager, so
// the value stays put as its width changes instead of the whole row shuffling.
static const int OVERLAY_TICK_SIDE_W = 76;
static const int OVERLAY_TICK_PREV_X = 6;     // 6..81
static const int OVERLAY_TICK_VALUE_X = 88;   // 88..211
static const int OVERLAY_TICK_VALUE_W = 124;
static const int OVERLAY_TICK_NEXT_X = 218;   // 218..293
static const int OVERLAY_TICK_VALUE_Y = 30;   // 48 -> 30..81
// Neighbours share the value's BASELINE, not its box top.
static const int OVERLAY_TICK_SIDE_Y =
  OVERLAY_TICK_VALUE_Y + (FONT48_ASCENT - FONT26_ASCENT);  // 49 -> 49..77
static const int OVERLAY_TICK_UNIT_Y = 84;    // 26 -> 84..112

// MODE: three tiles in a row, 4..95 / 104..195 / 204..295.
static const int OVERLAY_MODE_TILE_W = 92;
static const int OVERLAY_MODE_TILE_H = 56;
static const int OVERLAY_MODE_GAP = 8;
static const int OVERLAY_MODE_X0 = 4;
static const int OVERLAY_MODE_TILE_Y = 34;   // 34..89
// 14 centred in the tile: 34 + (56-16)/2 = 54.
static const int OVERLAY_MODE_LABEL_Y = 54;

// STOPS: the travel bar again, full panel width.
static const int OVERLAY_STOP_TRACK_X = 10;
static const int OVERLAY_STOP_TRACK_W = 280;  // 10..289
static const int OVERLAY_STOP_TRACK_Y = 34;   // 34..43
static const int OVERLAY_STOP_TRACK_H = 10;
static const int OVERLAY_STOP_MARK_Y = 30;    // 30..47, overhanging the track
static const int OVERLAY_STOP_MARK_H = 18;
static const int OVERLAY_STOP_MARK_W = 4;
static const int OVERLAY_STOP_CARRIAGE_W = 8;
static const int OVERLAY_STOP_LABEL_Y = 54;   // 14 -> 54..69
static const int OVERLAY_STOP_LEFT_X = 10;    // 10..99
static const int OVERLAY_STOP_RIGHT_X = 200;  // 200..289
static const int OVERLAY_STOP_END_W = 90;
static const int OVERLAY_STOP_POS_Y = 76;     // 26 -> 76..104
static const int OVERLAY_STOP_POS_X = 20;     // 20..279
static const int OVERLAY_STOP_POS_W = 260;
// The clear-both confirm bar (section 4: "STOPS hold - clear both, after a 1 s
// confirm bar"). It lives in the strip between the position readout (ends 105)
// and the body bottom (135) -- the one unused band of the STOPS body -- so the
// resting widget is not moved, resized or restyled by it: at rest all three
// objects are simply hidden and the overlay is pixel-identical to before.
// The words ride ON the bar (chipInk ink over the colourFault fill / the
// colourDisabled track -- 3.9:1 on the track, the worst case, same figure as
// the IDLE state chip) rather than beside it, so a glance catches the red, the
// growth and "CLEARING BOTH STOPS" as one object. Track geometry matches the
// travel track above it (same x, same width, RADIUS_TRACK) on purpose: the bar
// visibly "consumes" the same span whose two stops it is about to destroy.
static const int OVERLAY_STOP_CONFIRM_Y = 109;   // 109..132
static const int OVERLAY_STOP_CONFIRM_H = 24;
// 14 ink centred in the 24: 109 + (24 - 16) / 2 = 113.
static const int OVERLAY_STOP_CONFIRM_LABEL_Y = 113;
// The fill's floor width when the hold has only just started: 2 x RADIUS_TRACK,
// so the rounded rect never degenerates. Anything under ~1 tick of fill (28 px)
// renders as this nub.
static const int OVERLAY_STOP_CONFIRM_FILL_MIN = 8;

// MENU: the tile carousel (docs/ux-redesign.md section 6), three tiles across
// exactly as the mockup drew it:
//
//    40 +--- 2px accent border ----------------------+
//       |   MENU                             6 / 9   |  title + position (14)
//       |  +--------+  +==========+  +--------+      |
//       |  |  Sync  |  | Software |  |  Wi-Fi |      |  prev / CURRENT / next
//       |  |        |  |  update  |  |  setup |      |  (14, wrapped to 2 lines,
//       |  +--------+  +==========+  +--------+      |   centre = accent fill)
//       |   <> move                        OK open   |  hints          (14)
//   196 +--------------------------------------------+
//
// Three EQUAL tiles -- selection is marked by the accent FILL on the centre
// tile, never by size. That is only possible at Montserrat 14 with the names
// allowed to WRAP to two lines inside their tile ("Software / update"): the
// widest UNBREAKABLE word, "Diagnostics", is 85px at 14, so it is the word
// widths, not the full names, that size the tile (see TEXT14_MENU_WORD_W).
// Same geometry as the MODE tiles (92px tiles, 8px gaps, x0 4 -> 4..95 /
// 104..195 / 204..295 in the 300px content area), so the two three-across
// widgets read as one family.
static const int OVERLAY_MENU_POS_X = 236;   // 236..295, right of the title
static const int OVERLAY_MENU_POS_W = 60;
static const int OVERLAY_MENU_TILE_W = 92;
static const int OVERLAY_MENU_TILE_H = 60;
static const int OVERLAY_MENU_GAP = 8;
static const int OVERLAY_MENU_X0 = 4;
static const int OVERLAY_MENU_TILE_Y = 30;   // 30..89
static const int OVERLAY_MENU_NAME_PAD = 3;  // inset of the name box in its tile
                                             // -> inner width 86

// DRO DATUM: two tiles on the MODE grammar (same 92px tile, same 8px gap, same
// y band), centred because there are two of them, not three:
// x0 = (300 - (2*92 + 8)) / 2 = 54 -> tiles 54..145 / 154..245.
//
//    40 +--- 2px accent border ----------------------+
//       |               DRO DATUM                    |  title          (14)
//       |      +==========+    +----------+          |
//       |      |   LEFT   |    |  RIGHT   |          |  persisted end = accent
//       |      | I------  |    |  ------I |          |  mini bar, zero-post at
//       |      +==========+    +----------+          |  that end of travel
//       |   <> pick                        OK done   |  hints          (14)
//   204 +--------------------------------------------+
//
// Inside each tile: the name on top, and a MINIATURE travel bar below it with
// a zero-post at that tile's end -- the travel-bar metaphor is already
// established on the rest screen, so "which physical end reads 0.00" is shown,
// not just named. Bar + post are children of the tile (tile-relative
// coordinates), so the selection restyle recolours them with the tile's fill.
static const int OVERLAY_DATUM_TILE_W = 92;
static const int OVERLAY_DATUM_TILE_H = 56;
static const int OVERLAY_DATUM_GAP = 8;
static const int OVERLAY_DATUM_X0 =
  (OVERLAY_CONTENT_W - ((2 * OVERLAY_DATUM_TILE_W) + OVERLAY_DATUM_GAP)) / 2;
static const int OVERLAY_DATUM_TILE_Y = 34;   // 34..89
static const int OVERLAY_DATUM_LABEL_Y = 8;   // TILE-relative; 14 -> 8..24
// The mini bar, tile-relative: track 16..76 x 38..42, zero-post 14..20 (LEFT
// tile) / 72..78 (RIGHT tile) x 33..47, overhanging the track like the real
// stop markers do.
static const int OVERLAY_DATUM_BAR_X = 16;
static const int OVERLAY_DATUM_BAR_W = 60;
static const int OVERLAY_DATUM_BAR_Y = 38;
static const int OVERLAY_DATUM_BAR_H = 4;
static const int OVERLAY_DATUM_POST_W = 6;
static const int OVERLAY_DATUM_POST_H = 14;
static const int OVERLAY_DATUM_POST_Y = 33;
// Post x inside the tile: flush over the bar's outer end on each side.
static const int OVERLAY_DATUM_POST_X_LEFT = OVERLAY_DATUM_BAR_X - 2;
static const int OVERLAY_DATUM_POST_X_RIGHT =
  OVERLAY_DATUM_BAR_X + OVERLAY_DATUM_BAR_W - (OVERLAY_DATUM_POST_W - 2);

// --- Diagnostics screen ------------------------------------------------------
//
// A full 320x240 read-only screen (UiFocus::Diagnostics), built as an
// instrument panel: it exists to be WATCHED from arm's length while the
// machine runs (uistate.h exempts it from the idle timeout for exactly that
// reason), so the one number that diagnoses a lost step or a stiff slide --
// the live position error -- is the 48px hero, with a centre-zero deflection
// bar under it so drift reads as movement before the digits are legible.
//
//     0 +--------------------------------------------+
//       | DIAGNOSTICS            OK / MENU / HALT    |  title + exit hint (14)
//       |   POSITION ERROR                  +34 p    |  label + raw pulses (14)
//       |            +0.01  mm                       |  error   (48 + 26)
//       |  ------------------|------------------     |  centre-zero bar
//       +--------------------------------------------+  rule
//       |  SPINDLE      CARRIAGE      EXPECT         |  labels        (14)
//       |  320          6.72          6.72           |  values        (26)
//       |  RPM          mm/s          mm/s           |  units         (14)
//       |  SYNC [SYNCED]         ANCHOR   L stop     |  sync chip + anchor src
//   240 +--------------------------------------------+
//
// CARRIAGE vs EXPECT are deliberately the same unit (mm/s): a ratio problem
// then reads as two different numbers side by side, not as a thread that
// "looks wrong". EXPECT is |RPM|/60 x |pitch| and shows "--" unless the axis
// is engaged -- at rest the carriage legitimately does 0 and a live EXPECT
// would read as a fault. The arrows are inert on this screen and NOTHING here
// suggests otherwise; the only hint is the exit.
static const int DIAG_TITLE_X = 10;
static const int DIAG_TITLE_Y = 6;         // 14 -> 6..22
// The title's box runs up to the exit hint at DIAG_HINT_X (170), less a gap.
// It is fixed-width because the label is not static: it carries the capture
// status when a trace is running, and a shrink-to-fit box would re-lay-out
// under it on every change.
static const int DIAG_TITLE_W = 155;       // 10..165, clear of the hint at 170
static const int DIAG_ERR_LABEL_Y = 30;    // 14 -> 30..46
static const int DIAG_ERR_PULSES_X = 200;  // right-aligned box 200..310, row 1
static const int DIAG_ERR_PULSES_W = 110;
static const int DIAG_ERR_VALUE_X = 10;
static const int DIAG_ERR_VALUE_Y = 50;    // 48 -> 50..102
static const int DIAG_ERR_VALUE_W = 200;   // right-aligned, ink <= 194
static const int DIAG_ERR_UNIT_X = 216;    // "mm", baseline-aligned to the 48
static const int DIAG_ERR_UNIT_Y =
  DIAG_ERR_VALUE_Y + (FONT48_ASCENT - FONT26_ASCENT);  // 69 -> 69..97
// The centre-zero bar: full-scale deflection is +-DIAG_BAR_FULL_SCALE_MM, half
// a typical pitch -- an error past that is off any sane scale, so the marker
// pegs at the end and turns colourFault.
static const int DIAG_BAR_X = 10;
static const int DIAG_BAR_W = 300;
static const int DIAG_TICK_W = 2;          // the zero tick, tallest
static const int DIAG_TICK_Y = 108;        // 108..128
static const int DIAG_TICK_H = 20;
static const int DIAG_TICK_X = DIAG_BAR_X + ((DIAG_BAR_W - DIAG_TICK_W) / 2);
static const int DIAG_BAR_MARK_W = 8;
static const int DIAG_BAR_MARK_Y = 110;    // 110..126
static const int DIAG_BAR_MARK_H = 16;
static const int DIAG_BAR_TRACK_Y = 114;   // 114..122
static const int DIAG_BAR_TRACK_H = 8;
static const float DIAG_BAR_FULL_SCALE_MM = 0.5f;
static const int DIAG_RULE_Y = 132;
// The three rate columns.
static const int DIAG_COL_X0 = 10;
static const int DIAG_COL_X1 = 115;
static const int DIAG_COL_X2 = 220;
static const int DIAG_COL_W = 95;
static const int DIAG_RATE_LABEL_Y = 138;  // 14 -> 138..154
static const int DIAG_RATE_VALUE_Y = 157;  // 26 -> 157..186
static const int DIAG_RATE_UNIT_Y = 188;   // 14 -> 188..204
// Bottom row: sync chip left, the anchor SOURCE right. GlobalThreadSyncState
// (the chip) only says synced / not synced; getSyncAnchorState() says what the
// helix is pinned TO -- a stop, a manual sync point, or nothing -- which is
// the difference between a recut that picks up and one that does not. The two
// belong on one row because they are two halves of the same fact.
static const int DIAG_SYNC_LABEL_X = 10;
static const int DIAG_SYNC_CHIP_X = 64;    // ink x; the chip pads around it
static const int DIAG_BOTTOM_Y = 216;      // 14 ink -> 216..232
static const int DIAG_SYNC_PAD_H = 6;
static const int DIAG_SYNC_PAD_V = 3;      // chip 213..235
static const int DIAG_ANCHOR_LABEL_X = 180;  // "ANCHOR", dim -> 180..244
static const int DIAG_ANCHOR_VALUE_X = 250;  // right-aligned box 250..310
static const int DIAG_ANCHOR_VALUE_W = 60;
// The exit hint moved to the title row (its right half was empty) to free the
// bottom row for the anchor readout: chip box worst case ends at 165 and
// "OK / MENU / HALT" needs 129 of the remaining 145 -- the two cannot share.
static const int DIAG_HINT_X = 170;        // right-aligned box 170..310, row 1
static const int DIAG_HINT_W = 140;

// --- About screen ------------------------------------------------------------
//
// Quiet and plain (docs/ux-redesign.md section 6: "Firmware version, IP,
// uptime. Read-only"). The IP is the 36px hero: this is the screen someone
// stands at the machine to read an address off, so the address is the thing.
// Version and uptime share a row at 26. Exits like Diagnostics (OK/MENU/HALT,
// no timeout); arrows are inert and nothing suggests otherwise.
static const int ABOUT_TITLE_X = 10;
static const int ABOUT_TITLE_Y = 6;
static const int ABOUT_IP_LABEL_Y = 44;    // 14 -> 44..60
static const int ABOUT_IP_X = 10;
static const int ABOUT_IP_Y = 64;          // 36 -> 64..104
static const int ABOUT_ROW2_LABEL_Y = 130; // 14 -> 130..146
static const int ABOUT_ROW2_VALUE_Y = 150; // 26 -> 150..179
static const int ABOUT_FW_X = 10;
static const int ABOUT_FW_W = 150;         // 10..160
static const int ABOUT_UP_X = 170;
static const int ABOUT_UP_W = 140;         // 170..310
static const int ABOUT_HINT_X = 170;       // right-aligned box, bottom
static const int ABOUT_HINT_W = 140;
static const int ABOUT_HINT_Y = 216;

// --- Boot splash -------------------------------------------------------------
//
// A centred stack: mark, name, what the thing is, which firmware is running.
// Shown once per boot by showSplash(), for SPLASH_HOLD_MS (main.cpp), and never
// again -- so it carries no live values and needs no cache slots or object
// members. Everything on it is created local to the call and left to the
// lv_obj_clean() that init() opens with.
//
// Every text row is a FULL-WIDTH box with LV_TEXT_ALIGN_CENTER rather than a
// measured x. Centring three strings of three different lengths by hand means
// three measured ink widths that have to be re-measured whenever a word or a
// font changes -- and a wrong one is off-centre, not caught by any assert. The
// widths below are still recorded, but only as BOUNDS: they answer "does this
// fit", which is what an assert can actually check, and not "where does it go".
//
// The name is Montserrat 36, not 48. At 48 "HalfNut ELS" measures ~297 px of
// the 320 available -- the same no-margin overrun the Wi-Fi screens were pulled
// up on -- and the mark above it is doing the work a bigger wordmark would.
static const int SPLASH_LOGO_W = 96;       // halfNutLogo is 96x96
static const int SPLASH_LOGO_H = 96;
static const int SPLASH_LOGO_X = (SCREEN_W - SPLASH_LOGO_W) / 2;  // 112
static const int SPLASH_LOGO_Y = 16;       // 16..112
static const int SPLASH_NAME_Y = 122;      // 36 -> 122..162
static const int SPLASH_TAG_Y = 172;       // 14 -> 172..188
static const int SPLASH_VERSION_Y = 210;   // 14 -> 210..226, sat apart from the
                                           // block above it as a footer
// Measured worst-case ink, same summed-adv_w basis as the constants above.
static const int TEXT36_SPLASH_NAME_W = 224;  // "HalfNut ELS" at 36
static const int TEXT14_SPLASH_TAG_W = 168;   // "ELECTRONIC LEADSCREW" at 14
static const int TEXT14_SPLASH_VERSION_W = 60;  // "v10.10.10", well past any
                                                // version this will ever carry

// --- Wi-Fi setup screens -----------------------------------------------------
//
// showWifi() / showConnected() run BEFORE any stored config is valid -- that is
// why the device is in AP mode -- so there is no theme preference to read: the
// no-arg constructor pins m_palette to PALETTE_DARK and these screens are
// always dark, on the same palette tokens as everything else.
//
// Left column: the credentials as labelled text (the fallback if the QR can't
// be scanned). Right: the join QR on a WHITE CARD. The QR itself stays
// dark-on-light whatever the ground: inverted QR is readable by some scanners
// and not others, and this code exists to get a phone onto the AP -- not the
// place to be clever. The card's white padding is the quiet zone (lv_qrcode
// also centres the scaled code inside its canvas, so the canvas's own leftover
// margin adds to it), and black/white here are FUNCTIONAL, the one sanctioned
// pair of non-palette colours in this file.
static const int WIFI_LABEL_X = 10;
static const int WIFI_SSID_LABEL_Y = 12;   // 14 -> 12..28
static const int WIFI_SSID_VALUE_Y = 28;   // 26 -> 28..57
static const int WIFI_PASS_LABEL_Y = 82;   // 14 -> 82..98
static const int WIFI_PASS_VALUE_Y = 98;   // 26 -> 98..127
static const int WIFI_IP_LABEL_Y = 152;    // 14 -> 152..168
static const int WIFI_IP_VALUE_Y = 168;    // 26 -> 168..197
static const int WIFI_QR_SIZE = 124;       // the QR canvas (code + centring margin)
static const int WIFI_QR_PAD = 10;         // the white quiet-zone ring; ~2.3
                                           // modules at the payload's version 3
                                           // (29 modules over 124 px)
static const int WIFI_CARD_X = 170;        // card 170..313
static const int WIFI_CARD_Y = 42;         // card 42..185
static const int WIFI_CARD_W = WIFI_QR_SIZE + (2 * WIFI_QR_PAD);  // 144
static const int WIFI_SCAN_Y = 20;         // "Scan to join", 14 -> 20..36
// Credential values are boxed + clipped so a long string can never run under
// the card. The firmware's actual strings are far narrower (see the measured
// widths below); anything longer clips rather than colliding.
static const int WIFI_TEXT_W = WIFI_CARD_X - WIFI_LABEL_X - 6;  // 154

// The connected screen: what happened, what to do on a phone, and the AP
// address for computers. One column, no QR (deliberately -- see showConnected()).
static const int CONN_TITLE_Y = 14;        // 36 -> 14..54
static const int CONN_MSG_Y = 72;          // CONN_MSG_LINES x 14 -> 72..152
static const int CONN_MSG_LINES = 5;       // lines in showConnected()'s message
static const int CONN_IP_LABEL_Y = 178;    // 14 -> 178..194
static const int CONN_IP_VALUE_Y = 198;    // 26 -> 198..227

// Measured worst-case ink for the overlay's fixed boxes, on the same basis as
// the main screen's constants above (summed adv_w, no kerning credit).
// The 48 and 26 ticker widths are "6.00" -- the longest string ANY of the four
// tables can produce through formatPitch(): threadPitchMetric tops out at 6.00
// and feedPitchMetric at 0.75 (both 4 chars, and Montserrat's digits are all
// the same advance), threadPitchImperial renders as at most "80", the imperial
// feed table as at most "30" thou, and jogSpeeds as at most "100".
static const int TEXT48_TICKER_W = 105;    // "6.00" at 48
static const int TEXT26_TICKER_W = 56;     // "6.00" at 26
static const int TEXT26_TICKER_UNIT_W = 64;  // "thou", the longest unit word
static const int TEXT14_HINT_W = 171;      // "moving - datum locked", now the
                                           // longest left hint ("moving - stops
                                           // locked" is 161)
static const int TEXT14_HINT_OK_W = 64;    // "OK done"
static const int TEXT14_STOP_END_W = 69;   // "L -1200.00"
static const int TEXT26_STOP_POS_W = 158;  // "-1200.000 in"
static const int TEXT14_STOP_CONFIRM_W = 169;  // "CLEARING BOTH STOPS" at 14,
                                               // measured off the rendered ink
                                               // (pixel extent 75..243 in the
                                               // confirm-scene PNGs)
// Menu, on the same basis. Names WRAP inside their tile, so the binding width
// is the widest UNBREAKABLE word across the nine names in menuTileName() --
// "Diagnostics", 85px at 14 ("Software" is 65, every other word is narrower).
// The title is "MENU" and the position slot's widest reading is "9 / 9". These
// are what the carousel's boxes are checked against, so a longer word fails
// the build here rather than clipping silently on the panel.
static const int TEXT14_MENU_WORD_W = 85;    // "Diagnostics" at 14
static const int TEXT14_MENU_TITLE_W = 44;   // "MENU"
static const int TEXT14_MENU_POS_W = 31;     // "9 / 9"
// Datum picker, same measured-adv_w basis.
static const int TEXT14_DATUM_NAME_W = 44;   // "RIGHT" (wider than "LEFT", 34)
// Diagnostics. The 48 error is clamped by its formatter to +-999.99, so the
// widest ink it can EVER hold is "+999.99" -- 189, but "+300.00" measures 194
// because 3 is the widest digit; all digits share one advance, so the true
// bound is sign + 3 digits + point + 2 digits at the common advance = 194.
static const int TEXT48_DIAG_ERR_W = 194;    // "+300.00" at 48 (digit worst case)
static const int TEXT26_DIAG_MM_W = 56;      // "mm" at 26
static const int TEXT14_DIAG_PULSES_W = 67;  // "+99999 p" (formatter-clamped)
static const int TEXT14_DIAG_LABEL_W = 125;  // "POSITION ERROR"
static const int TEXT14_DIAG_COL_LABEL_W = 74;  // "CARRIAGE" (longest column label)
static const int TEXT26_DIAG_RATE_W = 80;    // "-99.99" (both mm/s columns;
                                             // "-9999" RPM is 74)
static const int TEXT14_DIAG_SYNC_STATE_W = 95;  // "NOT SYNCED"
static const int TEXT14_DIAG_HINT_W = 129;   // "OK / MENU / HALT"
static const int TEXT14_DIAG_TITLE_W = 100;  // "DIAGNOSTICS" -- now shares its
                                             // row with the exit hint, so its
                                             // ink is a bound, not just decor
static const int TEXT14_DIAG_ANCHOR_LABEL_W = 64;  // "ANCHOR"
static const int TEXT14_DIAG_ANCHOR_W = 55;  // "manual" (widest of the four
                                             // anchor sources; "R stop" is 46,
                                             // "L stop" 44, "none" 38)
// About.
static const int TEXT36_ABOUT_IP_W = 276;    // "255.255.255.255" at 36
                                             // ("not connected" is 272)
static const int TEXT26_ABOUT_FW_W = 139;    // "v99.99.999" -- a version longer
                                             // than that clips, it never crowds
static const int TEXT26_ABOUT_UP_W = 121;    // "999d 23h" (longest uptime form)
static const int TEXT14_ABOUT_HINT_W = 120;  // "OK / MENU close"
// Wi-Fi screens, same measured-adv_w basis.
static const int TEXT14_WIFI_SCAN_W = 85;    // "Scan to join"
static const int TEXT26_WIFI_VALUE_W = 137;  // worst realistic credential/IP
                                             // string: "123456789" (the AP
                                             // password) and the renderer's
                                             // "ELS-Setup" both measure 137;
                                             // "192.168.4.1" is 129,
                                             // "ELS_Wifi" 113
// OTA screen. The status label is auto-width, centred: its margin is what is
// left of the screen after the ink, so the widest string drawOTA() can push is
// the bound. "No update available" is 267; "Checking updates..." is 262 (its
// previous form, "Checking for updates...", was 306 -- flush to both edges).
static const int TEXT26_OTA_STATUS_W = 267;
static const int TEXT36_CONN_TITLE_W = 214;  // "Connected!"
static const int TEXT14_CONN_MSG_W = 256;    // "A device joined the setup
                                             // network." (widest message line)
static const int TEXT26_CONN_IP_W = 129;     // "192.168.4.1" (softAPIP)

// --- Layout assertions -------------------------------------------------------
// There is no host test for this file (lvgl is lib_ignore'd on the native env),
// so the band arithmetic is pinned here instead: every one of these is a claim
// made in the comments above, checked by the compiler on the device build.
static_assert(BAND_STATUS_BOTTOM < BAND_PITCH_BOTTOM, "bands out of order");
static_assert(BAND_PITCH_BOTTOM < BAND_TICKER_BOTTOM, "bands out of order");
static_assert(BAND_TICKER_BOTTOM < BAND_TRAVEL_BOTTOM, "bands out of order");
static_assert(BAND_TRAVEL_BOTTOM < SCREEN_H, "state band has no height");
// Band 1
static_assert(STATUS_CHIP_Y + FONT14_H <= BAND_STATUS_BOTTOM, "status chips overflow band 1");
static_assert(STATUS_RPM_VALUE_Y + FONT26_H <= BAND_STATUS_BOTTOM, "RPM value overflows band 1");
static_assert(STATUS_RPM_UNIT_Y + FONT14_H <= BAND_STATUS_BOTTOM, "RPM unit overflows band 1");
static_assert(STATUS_RPM_VALUE_X + STATUS_RPM_VALUE_W < STATUS_RPM_UNIT_X, "RPM value box hits its unit");
static_assert(STATUS_RPM_UNIT_X < SCREEN_W, "RPM unit off screen");
// Band 1 is a row of four unboxed/boxed items with no layout manager between
// them, so the only thing keeping them apart is these four x constants. Check
// them against the measured ink, not just against each other -- and against
// the SYNC label's chip padding, which extends its box beyond the ink on every
// side. sync chip -> RPM box is the tightest gap on the whole screen
// (185 vs 186).
static_assert(STATUS_MODE_X + TEXT14_MODE_W <= STATUS_UNIT_X, "mode text runs into the unit chip");
static_assert(STATUS_UNIT_X + TEXT14_UNIT_W <= STATUS_SYNC_X - SYNC_CHIP_PAD_H, "unit text runs into the sync chip");
static_assert(STATUS_SYNC_X + TEXT14_SYNC_W + SYNC_CHIP_PAD_H <= STATUS_RPM_VALUE_X, "sync chip runs into the RPM box");
static_assert(STATUS_CHIP_Y - SYNC_CHIP_PAD_V >= 0, "sync chip off the top of the screen");
static_assert(STATUS_CHIP_Y + FONT14_H + SYNC_CHIP_PAD_V <= BAND_STATUS_BOTTOM, "sync chip overflows band 1");
static_assert(STATUS_RPM_UNIT_X + TEXT14_RPM_UNIT_W <= SCREEN_W, "RPM unit text off the right edge");
static_assert(TEXT26_RPM_VALUE_W <= STATUS_RPM_VALUE_W, "RPM value wider than its box");
// Band 2
static_assert(PITCH_VALUE_Y > BAND_STATUS_BOTTOM, "pitch value overlaps band 1");
static_assert(PITCH_VALUE_Y + FONT48_H <= BAND_PITCH_BOTTOM, "pitch value overflows band 2");
static_assert(MODE_GLYPH_Y > BAND_STATUS_BOTTOM, "mode glyph overlaps band 1");
static_assert(MODE_GLYPH_Y + GLYPH_H <= BAND_PITCH_BOTTOM, "mode glyph overflows band 2");
static_assert(MODE_GLYPH_X + GLYPH_W <= SCREEN_W, "mode glyph off the right edge");
// Band 3 -- the pip row. Pips are bottom-aligned on PIP_BASE_Y and only their
// heights differ, so the vertical checks are on the TALLEST pip; the height
// ORDER is asserted because it is the entire visual encoding (current >
// neighbour > other) and a careless edit that flattened it would leave the
// row rendering but saying nothing.
static_assert(PIP_BASE_Y - PIP_H_CURRENT > BAND_PITCH_BOTTOM, "tallest pip overlaps band 2");
static_assert(PIP_BASE_Y < BAND_TICKER_BOTTOM, "pip baseline on or past the band rule");
static_assert(PIP_H_OTHER < PIP_H_NEIGHBOUR && PIP_H_NEIGHBOUR < PIP_H_CURRENT,
              "pip height hierarchy flattened - current/neighbour/other no longer distinguishable");
static_assert(PIP_ROW_X + PIP_ROW_W <= SCREEN_W, "pip row off the right edge");
// The reflow spreads pips across (PIP_ROW_W - PIP_W); that is only a scale if
// it is positive even for the longest list at the smallest spacing.
static_assert(PIP_W < PIP_ROW_W, "pip wider than its row");
// Band 4
static_assert(TRAVEL_MARK_Y > BAND_TICKER_BOTTOM, "travel markers overlap band 3");
static_assert(TRAVEL_MARK_Y + TRAVEL_MARK_H <= TRAVEL_VALUE_Y, "travel markers collide with the readout");
static_assert(TRAVEL_TRACK_Y >= TRAVEL_MARK_Y, "track sits above its markers");
static_assert(TRAVEL_TRACK_X + TRAVEL_TRACK_W <= SCREEN_W, "travel track off the right edge");
static_assert(TRAVEL_VALUE_Y + FONT26_H <= BAND_TRAVEL_BOTTOM, "travel value overflows band 4");
static_assert(TRAVEL_LABEL_Y + FONT14_H <= BAND_TRAVEL_BOTTOM, "travel labels overflow band 4");
// Was `TRAVEL_LEFT_X < TRAVEL_POS_X`, which is 12 < 90 and cannot fail for any
// plausible edit -- it asserted nothing about the label actually fitting. The
// left label is now boxed to TRAVEL_LEFT_W (like every other variable-length
// readout on this screen), so the real constraint is checkable.
static_assert(TRAVEL_LEFT_X + TRAVEL_LEFT_W < TRAVEL_POS_X, "left stop label box runs into the live readout");
static_assert(TRAVEL_POS_X + TRAVEL_POS_W < TRAVEL_POS_UNIT_X, "live readout runs into its unit");
static_assert(TRAVEL_POS_UNIT_X < TRAVEL_RIGHT_X, "live unit runs into the right stop label");
static_assert(TRAVEL_RIGHT_X + TRAVEL_RIGHT_W <= SCREEN_W, "right stop label off the right edge");
// Band 5 -- one row: the state chip. The label is auto-width (the chip fill
// must hug the word), so the right-edge check is against the measured ink of
// the longest word plus the chip's own padding, not against a box width.
static_assert(STATE_CHIP_Y > BAND_TRAVEL_BOTTOM, "state chip overlaps band 4");
static_assert(STATE_CHIP_Y + FONT26_H <= SCREEN_H, "state chip off the bottom");
static_assert(STATE_CHIP_X + (2 * STATE_CHIP_PAD_H) + TEXT26_STATE_W <= SCREEN_W,
              "widest state chip off the right edge");

// --- Selector overlay assertions ---------------------------------------------
// Same deal as above: no host test reaches this file, so the panel's arithmetic
// is checked by the compiler. The panel is opaque and sits on top of everything,
// so the first two are the ones that matter most -- an overlay that grew over
// either bar would hide machine state (RPM, or the CUTTING/HALTED word) behind
// a settings widget, which section 4 explicitly does not allow.
static_assert(OVERLAY_Y > BAND_STATUS_BOTTOM, "overlay covers the status bar");
// EQUALITY, not <=: the panel's bottom edge must land exactly on the band-4/5
// rule. Short leaves half-clipped travel glyphs poking out under the panel
// (looked like a rendering fault); long covers the state bar.
static_assert(OVERLAY_Y + OVERLAY_H == BAND_TRAVEL_BOTTOM, "overlay bottom edge is off the band-4/5 rule");
static_assert(OVERLAY_X + OVERLAY_W <= SCREEN_W, "overlay off the right edge");
static_assert(OVERLAY_X > 0, "overlay has no left margin");
// Shared rows. Everything below is in CONTENT coordinates, so the bound is the
// content size, not the panel size -- getting that wrong is exactly the mistake
// these catch (the border inset is 2px at each edge, i.e. 4 in each axis).
static_assert(OVERLAY_TITLE_Y + FONT14_H <= OVERLAY_BODY_TOP, "overlay title overlaps the body");
static_assert(OVERLAY_BODY_TOP < OVERLAY_BODY_BOTTOM, "overlay body has no height");
static_assert(OVERLAY_BODY_BOTTOM <= OVERLAY_HINT_Y, "overlay body overlaps the hint row");
// The left hint is auto-width with a permanent chip padding (see the constants
// above), so its collision checks are ink + padding, not a fixed box.
static_assert(OVERLAY_HINT_Y - OVERLAY_HINT_PAD_V >= OVERLAY_BODY_BOTTOM, "hint chip overlaps the body");
static_assert(OVERLAY_HINT_Y + FONT14_H + OVERLAY_HINT_PAD_V <= OVERLAY_CONTENT_H,
              "overlay hint chip falls off the panel");
static_assert(OVERLAY_HINT_L_X + TEXT14_HINT_W + OVERLAY_HINT_PAD_H <= OVERLAY_HINT_R_X,
              "longest left hint chip runs into the right hint");
static_assert(OVERLAY_HINT_L_X - OVERLAY_HINT_PAD_H >= 0, "hint chip off the left of the panel");
static_assert(OVERLAY_HINT_R_X + OVERLAY_HINT_R_W <= OVERLAY_CONTENT_W, "overlay right hint off the panel");
static_assert(TEXT14_HINT_OK_W <= OVERLAY_HINT_R_W, "\"OK done\" wider than its box");
// Ticker. The neighbour boxes are the only thing stopping a long neighbour
// value from running under the 48, so check the ink, not just the boxes.
static_assert(OVERLAY_TICK_PREV_X + OVERLAY_TICK_SIDE_W <= OVERLAY_TICK_VALUE_X, "ticker neighbour runs into the value");
static_assert(OVERLAY_TICK_VALUE_X + OVERLAY_TICK_VALUE_W <= OVERLAY_TICK_NEXT_X, "ticker value runs into its neighbour");
static_assert(OVERLAY_TICK_NEXT_X + OVERLAY_TICK_SIDE_W <= OVERLAY_CONTENT_W, "ticker off the right of the panel");
static_assert(TEXT48_TICKER_W <= OVERLAY_TICK_VALUE_W, "ticker value wider than its box");
static_assert(TEXT26_TICKER_W <= OVERLAY_TICK_SIDE_W, "ticker neighbour wider than its box");
static_assert(TEXT26_TICKER_UNIT_W <= OVERLAY_TICK_VALUE_W, "ticker unit wider than its box");
static_assert(OVERLAY_TICK_VALUE_Y >= OVERLAY_BODY_TOP, "ticker value overlaps the title");
static_assert(OVERLAY_TICK_VALUE_Y + FONT48_H <= OVERLAY_TICK_UNIT_Y, "ticker value overlaps its unit");
static_assert(OVERLAY_TICK_UNIT_Y + FONT26_H <= OVERLAY_BODY_BOTTOM, "ticker unit overflows the body");
// The neighbours are baseline-aligned to the value, which means their box tops
// differ by the ascent difference. Written out rather than left implicit in the
// initialiser so that changing either font size fails here.
static_assert(OVERLAY_TICK_SIDE_Y - OVERLAY_TICK_VALUE_Y == FONT48_ASCENT - FONT26_ASCENT,
              "ticker neighbours are off the value's baseline");
static_assert(OVERLAY_TICK_SIDE_Y + FONT26_H <= OVERLAY_TICK_UNIT_Y, "ticker neighbours overlap the unit");
// MODE tiles.
static_assert(OVERLAY_MODE_X0 + (3 * OVERLAY_MODE_TILE_W) + (2 * OVERLAY_MODE_GAP) <= OVERLAY_CONTENT_W,
              "mode tiles off the right of the panel");
static_assert(TEXT14_MODE_W <= OVERLAY_MODE_TILE_W, "\"THREAD R\" wider than a mode tile");
static_assert(OVERLAY_MODE_TILE_Y >= OVERLAY_BODY_TOP, "mode tiles overlap the title");
static_assert(OVERLAY_MODE_TILE_Y + OVERLAY_MODE_TILE_H <= OVERLAY_BODY_BOTTOM, "mode tiles overflow the body");
static_assert(OVERLAY_MODE_LABEL_Y >= OVERLAY_MODE_TILE_Y &&
              OVERLAY_MODE_LABEL_Y + FONT14_H <= OVERLAY_MODE_TILE_Y + OVERLAY_MODE_TILE_H,
              "mode tile label is not inside its tile");
// STOPS.
static_assert(OVERLAY_STOP_TRACK_X + OVERLAY_STOP_TRACK_W <= OVERLAY_CONTENT_W, "stops track off the right of the panel");
static_assert(OVERLAY_STOP_MARK_Y >= OVERLAY_BODY_TOP, "stops markers overlap the title");
static_assert(OVERLAY_STOP_MARK_Y <= OVERLAY_STOP_TRACK_Y &&
              OVERLAY_STOP_TRACK_Y + OVERLAY_STOP_TRACK_H <= OVERLAY_STOP_MARK_Y + OVERLAY_STOP_MARK_H,
              "stops markers no longer overhang the track");
static_assert(OVERLAY_STOP_MARK_Y + OVERLAY_STOP_MARK_H <= OVERLAY_STOP_LABEL_Y, "stops markers collide with the end labels");
static_assert(OVERLAY_STOP_LABEL_Y + FONT14_H <= OVERLAY_STOP_POS_Y, "stops end labels collide with the live readout");
static_assert(OVERLAY_STOP_POS_Y + FONT26_H <= OVERLAY_BODY_BOTTOM, "stops readout overflows the body");
static_assert(OVERLAY_STOP_LEFT_X + OVERLAY_STOP_END_W <= OVERLAY_STOP_RIGHT_X, "stops end labels collide");
static_assert(OVERLAY_STOP_RIGHT_X + OVERLAY_STOP_END_W <= OVERLAY_CONTENT_W, "stops right label off the panel");
static_assert(TEXT14_STOP_END_W <= OVERLAY_STOP_END_W, "stop end label wider than its box");
static_assert(OVERLAY_STOP_POS_X + OVERLAY_STOP_POS_W <= OVERLAY_CONTENT_W, "stops readout off the panel");
static_assert(TEXT26_STOP_POS_W <= OVERLAY_STOP_POS_W, "stops readout wider than its box");
// The carriage must have somewhere to travel: it is placed by fraction across
// (track width - its own width), which is only a scale if that is positive.
static_assert(OVERLAY_STOP_CARRIAGE_W < OVERLAY_STOP_TRACK_W, "carriage marker wider than the track");
// The clear-both confirm bar: it borrows the strip UNDER the position readout,
// so it must clear the readout above and the body bottom below -- and its label
// rides inside it, ink fully within the bar's height and narrower than the
// track it is centred on.
static_assert(OVERLAY_STOP_CONFIRM_Y >= OVERLAY_STOP_POS_Y + FONT26_H,
              "confirm bar overlaps the position readout");
static_assert(OVERLAY_STOP_CONFIRM_Y + OVERLAY_STOP_CONFIRM_H <= OVERLAY_BODY_BOTTOM,
              "confirm bar overflows the body into the hint row");
static_assert(OVERLAY_STOP_CONFIRM_LABEL_Y >= OVERLAY_STOP_CONFIRM_Y &&
              OVERLAY_STOP_CONFIRM_LABEL_Y + FONT14_H <=
                OVERLAY_STOP_CONFIRM_Y + OVERLAY_STOP_CONFIRM_H,
              "confirm label ink outside its bar");
static_assert(TEXT14_STOP_CONFIRM_W <= OVERLAY_STOP_TRACK_W,
              "\"CLEARING BOTH STOPS\" wider than the confirm bar");
// The fill scales permille over the track width, and its floor must not exceed
// what a full bar can be, or a just-started hold would render fuller than the
// arithmetic says.
static_assert(OVERLAY_STOP_CONFIRM_FILL_MIN <= OVERLAY_STOP_TRACK_W,
              "confirm fill floor wider than its track");
// MENU carousel.
// The title label is a FULL-CONTENT-WIDTH centred box, so it geometrically
// overlaps the position box; what must not overlap is the INK. A centred string
// of width w occupies (CONTENT_W - w)/2 .. (CONTENT_W + w)/2, so the title's
// right ink edge is the bound the position slot has to clear. Asserting the two
// boxes instead would be vacuously true and would prove nothing.
static_assert(((OVERLAY_CONTENT_W + TEXT14_MENU_TITLE_W) / 2) <= OVERLAY_MENU_POS_X,
              "MENU title ink runs into the n/9 position slot");
static_assert(OVERLAY_MENU_POS_X + OVERLAY_MENU_POS_W <= OVERLAY_CONTENT_W,
              "menu position slot off the right of the panel");
static_assert(TEXT14_MENU_POS_W <= OVERLAY_MENU_POS_W, "\"9 / 9\" wider than its box");
// Three equal tiles with gaps must fit the content area (4 + 3*92 + 2*8 = 296
// in 300), and each must clear its vertical bounds.
static_assert(OVERLAY_MENU_X0 + (3 * OVERLAY_MENU_TILE_W) + (2 * OVERLAY_MENU_GAP) <= OVERLAY_CONTENT_W,
              "menu tiles off the right of the panel");
static_assert(OVERLAY_MENU_TILE_Y >= OVERLAY_BODY_TOP, "menu tiles overlap the title row");
static_assert(OVERLAY_MENU_TILE_Y + OVERLAY_MENU_TILE_H <= OVERLAY_BODY_BOTTOM,
              "menu tiles overflow the body into the hints");
// Names wrap to at most two lines inside their tile, so the constraints are
// (a) the widest unbreakable WORD fits the inner width -- otherwise LVGL
// letter-breaks it mid-word -- and (b) two wrapped lines fit the tile height.
// The labels are CHILDREN of the tiles (clipped + kept centred by LVGL), so
// these two are what keep a wrapped name whole and inside its tile.
static_assert(TEXT14_MENU_WORD_W <= OVERLAY_MENU_TILE_W - (2 * OVERLAY_MENU_NAME_PAD),
              "widest menu word wider than a tile's inner box");
static_assert((2 * FONT14_H) <= OVERLAY_MENU_TILE_H - (2 * OVERLAY_MENU_NAME_PAD),
              "a two-line menu name overflows its tile");
// DRO DATUM picker.
// The centring is a RELATIONSHIP, not a number: if anyone edits the tile
// width or the gap without re-deriving x0, this is what fails.
static_assert(OVERLAY_DATUM_X0 ==
              (OVERLAY_CONTENT_W - ((2 * OVERLAY_DATUM_TILE_W) + OVERLAY_DATUM_GAP)) / 2,
              "datum tiles are no longer centred in the panel");
static_assert(OVERLAY_DATUM_X0 + (2 * OVERLAY_DATUM_TILE_W) + OVERLAY_DATUM_GAP <= OVERLAY_CONTENT_W,
              "datum tiles off the right of the panel");
static_assert(OVERLAY_DATUM_TILE_Y >= OVERLAY_BODY_TOP, "datum tiles overlap the title");
static_assert(OVERLAY_DATUM_TILE_Y + OVERLAY_DATUM_TILE_H <= OVERLAY_BODY_BOTTOM,
              "datum tiles overflow the body");
static_assert(TEXT14_DATUM_NAME_W <= OVERLAY_DATUM_TILE_W, "\"RIGHT\" wider than a datum tile");
// Everything inside the tile is TILE-relative and the tile clips its children,
// so an out-of-bounds child disappears silently -- these are what catch that.
static_assert(OVERLAY_DATUM_LABEL_Y + FONT14_H <= OVERLAY_DATUM_POST_Y,
              "datum name collides with the zero-post");
static_assert(OVERLAY_DATUM_POST_Y <= OVERLAY_DATUM_BAR_Y &&
              OVERLAY_DATUM_BAR_Y + OVERLAY_DATUM_BAR_H <= OVERLAY_DATUM_POST_Y + OVERLAY_DATUM_POST_H,
              "datum zero-post no longer overhangs the mini bar");
static_assert(OVERLAY_DATUM_POST_Y + OVERLAY_DATUM_POST_H <= OVERLAY_DATUM_TILE_H,
              "datum zero-post clipped by its tile");
static_assert(OVERLAY_DATUM_POST_X_LEFT >= 0 &&
              OVERLAY_DATUM_POST_X_RIGHT + OVERLAY_DATUM_POST_W <= OVERLAY_DATUM_TILE_W,
              "datum zero-post clipped at a tile edge");
static_assert(OVERLAY_DATUM_BAR_X + OVERLAY_DATUM_BAR_W <= OVERLAY_DATUM_TILE_W,
              "datum mini bar clipped by its tile");

// --- Diagnostics screen assertions -------------------------------------------
// Row order, top to bottom, each row's box against the next -- a full screen
// with no band rules to inherit, so the whole vertical chain is stated.
static_assert(DIAG_TITLE_Y + FONT14_H <= DIAG_ERR_LABEL_Y, "diag title overlaps the error label");
static_assert(DIAG_ERR_LABEL_Y + FONT14_H <= DIAG_ERR_VALUE_Y, "diag error label overlaps its value");
static_assert(DIAG_ERR_VALUE_Y + FONT48_H <= DIAG_TICK_Y, "diag error value overlaps the bar");
static_assert(DIAG_TICK_Y + DIAG_TICK_H <= DIAG_RULE_Y, "diag bar overlaps the rule");
static_assert(DIAG_RULE_Y < DIAG_RATE_LABEL_Y, "diag rule below the rate labels");
static_assert(DIAG_RATE_LABEL_Y + FONT14_H <= DIAG_RATE_VALUE_Y, "diag rate label overlaps its value");
static_assert(DIAG_RATE_VALUE_Y + FONT26_H <= DIAG_RATE_UNIT_Y, "diag rate value overlaps its unit");
static_assert(DIAG_RATE_UNIT_Y + FONT14_H <= DIAG_BOTTOM_Y - DIAG_SYNC_PAD_V,
              "diag rate units collide with the sync chip");
static_assert(DIAG_BOTTOM_Y + FONT14_H + DIAG_SYNC_PAD_V <= SCREEN_H, "diag sync chip off the bottom");
// The centre-zero bar. The tick must actually BE at zero deflection -- stated
// as the derivation so an edited x fails here rather than lying on screen --
// and the marker/track/tick nest exactly as the travel bar's marks do.
static_assert(DIAG_TICK_X == DIAG_BAR_X + ((DIAG_BAR_W - DIAG_TICK_W) / 2),
              "diag zero tick is not at the centre of the bar");
static_assert(DIAG_TICK_Y <= DIAG_BAR_MARK_Y && DIAG_BAR_MARK_Y <= DIAG_BAR_TRACK_Y &&
              DIAG_BAR_TRACK_Y + DIAG_BAR_TRACK_H <= DIAG_BAR_MARK_Y + DIAG_BAR_MARK_H &&
              DIAG_BAR_MARK_Y + DIAG_BAR_MARK_H <= DIAG_TICK_Y + DIAG_TICK_H,
              "diag bar tick/marker/track no longer nest");
static_assert(DIAG_BAR_MARK_W < DIAG_BAR_W, "diag marker wider than its bar");
static_assert(DIAG_BAR_X + DIAG_BAR_W <= SCREEN_W, "diag bar off the right edge");
// Horizontal ink. The error value is right-aligned in a fixed box; its unit
// hangs at a fixed x past it; the raw-pulses slot shares the title row.
static_assert(TEXT48_DIAG_ERR_W <= DIAG_ERR_VALUE_W, "clamped diag error wider than its box");
static_assert(DIAG_ERR_VALUE_X + DIAG_ERR_VALUE_W <= DIAG_ERR_UNIT_X, "diag error value runs into its unit");
static_assert(DIAG_ERR_UNIT_X + TEXT26_DIAG_MM_W <= SCREEN_W, "diag error unit off the right edge");
static_assert(DIAG_ERR_UNIT_Y - DIAG_ERR_VALUE_Y == FONT48_ASCENT - FONT26_ASCENT,
              "diag error unit off the value's baseline");
static_assert(DIAG_TITLE_X + TEXT14_DIAG_LABEL_W <= DIAG_ERR_PULSES_X,
              "diag title row ink runs into the pulses slot");
static_assert(TEXT14_DIAG_PULSES_W <= DIAG_ERR_PULSES_W, "clamped pulses wider than their box");
static_assert(DIAG_ERR_PULSES_X + DIAG_ERR_PULSES_W <= SCREEN_W, "diag pulses box off the right edge");
// The three columns: boxes must not collide AND the worst measured ink must
// fit a column, or a negative rate silently overprints its neighbour.
static_assert(DIAG_COL_X0 + DIAG_COL_W <= DIAG_COL_X1 && DIAG_COL_X1 + DIAG_COL_W <= DIAG_COL_X2,
              "diag rate columns collide");
static_assert(DIAG_COL_X2 + DIAG_COL_W <= SCREEN_W, "diag third column off the right edge");
static_assert(TEXT26_DIAG_RATE_W <= DIAG_COL_W, "worst rate value wider than its column");
static_assert(TEXT14_DIAG_COL_LABEL_W <= DIAG_COL_W, "\"CARRIAGE\" wider than its column");
// Title row: the exit hint lives up here now, so the title's INK is a bound
// the hint box has to clear (same reasoning as the MENU title assert above --
// comparing the two boxes would prove nothing about the text between them).
static_assert(DIAG_TITLE_X + TEXT14_DIAG_TITLE_W <= DIAG_HINT_X,
              "DIAGNOSTICS title ink runs into the exit hint");
static_assert(TEXT14_DIAG_HINT_W <= DIAG_HINT_W, "diag exit hint wider than its box");
static_assert(DIAG_HINT_X + DIAG_HINT_W <= SCREEN_W, "diag hint box off the right edge");
// Bottom row: the sync chip's padded box, the ANCHOR label's ink and the
// anchor value's box, left to right. Ink bounds where the text is what can
// collide; the chip pads beyond its ink on every side.
static_assert(DIAG_SYNC_CHIP_X + TEXT14_DIAG_SYNC_STATE_W + DIAG_SYNC_PAD_H <= DIAG_ANCHOR_LABEL_X,
              "\"NOT SYNCED\" chip runs into the ANCHOR label");
static_assert(DIAG_ANCHOR_LABEL_X + TEXT14_DIAG_ANCHOR_LABEL_W <= DIAG_ANCHOR_VALUE_X,
              "ANCHOR label ink runs into the anchor value box");
static_assert(TEXT14_DIAG_ANCHOR_W <= DIAG_ANCHOR_VALUE_W,
              "\"manual\" wider than the anchor value box");
static_assert(DIAG_ANCHOR_VALUE_X + DIAG_ANCHOR_VALUE_W <= SCREEN_W,
              "anchor value box off the right edge");

// --- About screen assertions --------------------------------------------------
static_assert(ABOUT_TITLE_Y + FONT14_H <= ABOUT_IP_LABEL_Y, "about title overlaps the IP label");
static_assert(ABOUT_IP_LABEL_Y + FONT14_H <= ABOUT_IP_Y, "about IP label overlaps the address");
static_assert(ABOUT_IP_Y + FONT36_H <= ABOUT_ROW2_LABEL_Y, "about IP overlaps the second row");
static_assert(ABOUT_ROW2_LABEL_Y + FONT14_H <= ABOUT_ROW2_VALUE_Y, "about row-2 labels overlap their values");
static_assert(ABOUT_ROW2_VALUE_Y + FONT26_H <= ABOUT_HINT_Y, "about row 2 overlaps the exit hint");
static_assert(ABOUT_HINT_Y + FONT14_H <= SCREEN_H, "about hint off the bottom");
// The IP is the widest thing on the screen and is NOT boxed (it must never
// clip -- a truncated IP is worse than none), so the check is measured ink
// against the screen edge for the widest address that exists.
static_assert(ABOUT_IP_X + TEXT36_ABOUT_IP_W <= SCREEN_W, "\"255.255.255.255\" off the right edge");
static_assert(ABOUT_FW_X + ABOUT_FW_W <= ABOUT_UP_X, "about version box runs into the uptime box");
static_assert(ABOUT_UP_X + ABOUT_UP_W <= SCREEN_W, "about uptime box off the right edge");
static_assert(TEXT26_ABOUT_FW_W <= ABOUT_FW_W, "\"v99.99.999\" wider than the version box");
static_assert(TEXT26_ABOUT_UP_W <= ABOUT_UP_W, "\"999d 23h\" wider than the uptime box");
static_assert(TEXT14_ABOUT_HINT_W <= ABOUT_HINT_W, "about exit hint wider than its box");

// --- Wi-Fi screen assertions ---------------------------------------------------
// The card is square (the QR is), so one width serves both axes.
// Boot splash. The rows are centred in full-width boxes, so what has to be
// checked is that each row FITS (a row wider than the screen would clip at both
// ends, since it is centred) and that the four of them stack without touching.
static_assert(SPLASH_LOGO_X >= 0, "splash mark wider than the screen");
static_assert(SPLASH_LOGO_Y + SPLASH_LOGO_H <= SPLASH_NAME_Y, "splash mark overlaps the name");
static_assert(SPLASH_NAME_Y + FONT36_H <= SPLASH_TAG_Y, "splash name overlaps the tagline");
static_assert(SPLASH_TAG_Y + FONT14_H <= SPLASH_VERSION_Y, "splash tagline overlaps the version");
static_assert(SPLASH_VERSION_Y + FONT14_H <= SCREEN_H, "splash version off the bottom");
static_assert(TEXT36_SPLASH_NAME_W <= SCREEN_W, "splash name wider than the screen");
static_assert(TEXT14_SPLASH_TAG_W <= SCREEN_W, "splash tagline wider than the screen");
static_assert(TEXT14_SPLASH_VERSION_W <= SCREEN_W, "splash version wider than the screen");

static_assert(WIFI_CARD_X + WIFI_CARD_W <= SCREEN_W, "QR card off the right edge");
static_assert(WIFI_CARD_Y + WIFI_CARD_W <= SCREEN_H, "QR card off the bottom");
static_assert(WIFI_SCAN_Y + FONT14_H <= WIFI_CARD_Y, "\"Scan to join\" overlaps the QR card");
static_assert(TEXT14_WIFI_SCAN_W <= WIFI_CARD_W, "\"Scan to join\" wider than the card it captions");
// The credential column is boxed + clipped; the box must clear the card, and
// the worst realistic string must fit the box (a wider one clips, it never
// collides).
static_assert(WIFI_LABEL_X + WIFI_TEXT_W < WIFI_CARD_X, "credential text box runs under the QR card");
static_assert(TEXT26_WIFI_VALUE_W <= WIFI_TEXT_W, "worst credential string wider than its box");
// The three label/value pairs, top to bottom.
static_assert(WIFI_SSID_LABEL_Y + FONT14_H <= WIFI_SSID_VALUE_Y, "SSID label overlaps its value");
static_assert(WIFI_SSID_VALUE_Y + FONT26_H <= WIFI_PASS_LABEL_Y, "SSID value overlaps the password label");
static_assert(WIFI_PASS_LABEL_Y + FONT14_H <= WIFI_PASS_VALUE_Y, "password label overlaps its value");
static_assert(WIFI_PASS_VALUE_Y + FONT26_H <= WIFI_IP_LABEL_Y, "password value overlaps the IP label");
static_assert(WIFI_IP_LABEL_Y + FONT14_H <= WIFI_IP_VALUE_Y, "IP label overlaps its value");
static_assert(WIFI_IP_VALUE_Y + FONT26_H <= SCREEN_H, "IP value off the bottom");
// The connected screen: title, the wrapped message (all CONN_MSG_LINES of it),
// then the labelled address.
static_assert(CONN_TITLE_Y + FONT36_H <= CONN_MSG_Y, "connected title overlaps the message");
static_assert(CONN_MSG_Y + (CONN_MSG_LINES * FONT14_H) <= CONN_IP_LABEL_Y,
              "connected message overlaps the IP label");
static_assert(CONN_IP_LABEL_Y + FONT14_H <= CONN_IP_VALUE_Y, "connected IP label overlaps the address");
static_assert(CONN_IP_VALUE_Y + FONT26_H <= SCREEN_H, "connected IP off the bottom");
static_assert(WIFI_LABEL_X + TEXT36_CONN_TITLE_W <= SCREEN_W, "\"Connected!\" off the right edge");
static_assert(WIFI_LABEL_X + TEXT14_CONN_MSG_W <= SCREEN_W, "connected message off the right edge");
static_assert(WIFI_LABEL_X + TEXT26_CONN_IP_W <= SCREEN_W, "connected IP off the right edge");

// --- OTA screen assertion ------------------------------------------------------
// The status label is centred and auto-width, so a margin of at least 20 px a
// side is a claim about the widest STRING, not about any box constant -- which
// is exactly why the "Checking for updates..." regression (306 px of ink in
// 320) got through: nothing measured the string. Now something does.
static_assert(TEXT26_OTA_STATUS_W <= SCREEN_W - (2 * 20),
              "widest OTA status line leaves no side margin");

// Radii. LV_DRAW_SW_CIRCLE_CACHE_SIZE is 4, so keep the number of DISTINCT
// radii small (docs/ux-redesign.md section 8 "Renderer constraints"): this
// screen uses exactly three -- 0 (rules, band fills), 4 (tracks and markers)
// and LV_RADIUS_CIRCLE (the state dot).
static const int RADIUS_TRACK = 4;

static uint32_t my_tick(void) {
  return millis();
}

// --- Small object-construction helpers ---------------------------------------
// lv_obj_create() inherits the default theme's panel styling (border, radius,
// padding, scrolling). Everything on this screen is a flat, opaque, unpadded
// rectangle at an absolute position, so strip all of that once here rather than
// at every call site. No shadow and no bg gradient is set anywhere: shadows are
// uncached on this build and complex gradients are compiled out.
static lv_obj_t* createRect(lv_obj_t* parent, int x, int y, int w, int h,
                            lv_color_t colour, int radius) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(obj, colour, 0);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_pos(obj, x, y);
  return obj;
}

static lv_obj_t* createLabel(lv_obj_t* parent, const lv_font_t* font,
                             lv_color_t colour, int x, int y) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);
  lv_label_set_text(label, "");
  lv_obj_set_pos(label, x, y);
  return label;
}

// A label with a FIXED width and an alignment inside it. Used wherever the text
// must not creep into a neighbour as its length changes (the RPM and travel
// readouts, the soft-key columns). LONG_MODE_CLIP rather than the default WRAP:
// if a value ever does exceed the box, clipping it keeps the single-line layout
// instead of silently growing the label downwards into the next band.
// One switchable content group inside the selector overlay: an invisible box
// filling the panel's content area, whose only job is to be shown or hidden as
// a unit. Sized and positioned so its children's coordinates are the panel's
// content coordinates unchanged (pad 0, border 0 -> content origin == origin).
//
// bg_opa TRANSP is NOT translucency in the sense section 8 forbids: it skips
// drawing the background entirely. Only LV_STYLE_OPA (whole-object opacity)
// forces LVGL to allocate an intermediate layer, and nothing here sets it.
static lv_obj_t* createOverlayGroup(lv_obj_t* parent) {
  lv_obj_t* group = lv_obj_create(parent);
  lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(group, 0, 0);
  lv_obj_set_style_pad_all(group, 0, 0);
  lv_obj_set_style_radius(group, 0, 0);
  lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
  lv_obj_set_size(group, OVERLAY_CONTENT_W, OVERLAY_CONTENT_H);
  lv_obj_set_pos(group, 0, 0);
  lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
  return group;
}

static void fixLabelBox(lv_obj_t* label, int width, lv_text_align_t align) {
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_style_text_align(label, align, 0);
}

// Formats a carriage/stop position for display. docs/ux-redesign.md section 8
// "Units": metric reads mm to 2 dp, imperial reads INCHES to 3 dp (feed pitch
// is thou/rev in the same mode -- that mismatch is deliberate, it is what
// machine tools do). Positions are held in pulses and converted here only.
static void formatTravelValue(char* buf, size_t len, float millimetres,
                              bool imperial) {
  if (imperial) {
    snprintf(buf, len, "%.3f", (double)(millimetres / 25.4f));
  } else {
    snprintf(buf, len, "%.2f", (double)millimetres);
  }
}

// --- Pitch / jog-speed tables ------------------------------------------------
//
// ONE place decides which table the current (feed mode x unit mode) pair
// selects and how a value out of it is rendered. drawPitch() (the main readout)
// and the RATE / JOG SPEED overlays all go through here, so they cannot drift
// apart -- previously the whole if/else chain lived inside drawPitch().
//
// It is NOT legitimate to render GlobalState::getCurrentFeedPitch() instead:
// that returns mm/rev in EVERY mode (so imperial would show the metric
// equivalent) and it negates for FM_THREAD_REVERSE. Note also that
// feedPitchImperial[] is commented thou/rev but actually holds INCHES -- the
// x1000 in PF_THOU is what makes this readout correct, and it is deliberately
// left in place; the array's mislabelling (and getCurrentFeedPitch()'s
// consequent 1000x error) is tracked separately and must not be "fixed" here.
enum PitchFormat {
  PF_MM,       // metric pitch, mm/rev to 2 dp
  PF_TPI,      // imperial thread, whole TPI
  PF_THOU,     // imperial feed, inches in the table -> thou on screen
  PF_PERCENT,  // jog speed, a 0..1 fraction shown as a percentage
};

struct PitchTable {
  const float* values;
  int count;
  const char* unit;  // static literal; never formatted into a buffer
  PitchFormat format;
};

// jogSpeeds[] is the JOG SPEED widget's list whatever the feed mode is, which
// is why this is reachable independently of currentPitchTable().
static PitchTable jogSpeedTable() {
  PitchTable table;
  table.values = jogSpeeds;
  table.count = (int)ARRAY_SIZE(jogSpeeds);
  table.unit = "%";
  table.format = PF_PERCENT;
  return table;
}

static PitchTable currentPitchTable(GlobalFeedMode mode, GlobalUnitMode unit) {
  if (mode == FM_JOG) {
    // FM_JOG is on its way out of the mode cycle (section 3), but it is still a
    // value GlobalState can hold, so the main readout still has to render it.
    return jogSpeedTable();
  }

  const bool thread = (mode == FM_THREAD || mode == FM_THREAD_REVERSE);
  PitchTable table;
  if (unit == METRIC) {
    table.values = thread ? threadPitchMetric : feedPitchMetric;
    table.count = thread ? (int)ARRAY_SIZE(threadPitchMetric)
                         : (int)ARRAY_SIZE(feedPitchMetric);
    table.unit = "mm";
    table.format = PF_MM;
  } else if (thread) {
    table.values = threadPitchImperial;
    table.count = (int)ARRAY_SIZE(threadPitchImperial);
    table.unit = "TPI";
    table.format = PF_TPI;
  } else {
    table.values = feedPitchImperial;
    table.count = (int)ARRAY_SIZE(feedPitchImperial);
    table.unit = "thou";
    table.format = PF_THOU;
  }
  return table;
}

// Renders one entry of `table`. An out-of-range index yields an EMPTY string
// rather than reading off the end of the array: the overlay ticker asks for
// index-1 and index+1 on purpose, and a blank neighbour is exactly the right
// rendering at the ends of the list. (It also stops a stale m_feedSelect from
// indexing a shorter table, which the old inline code would have done.)
//
// The two integer conversions round (+0.5f) where the previous inline code
// truncated. Every value in all five tables is unchanged by that -- the nearest
// float to each happens to land just above its integer target -- but truncation
// only ever worked by luck, and one edited table entry would have shown 13 thou
// as "12".
static void formatPitch(char* buf, size_t len, const PitchTable& table,
                        int index) {
  if (index < 0 || index >= table.count) {
    buf[0] = '\0';
    return;
  }
  const float value = table.values[index];
  switch (table.format) {
  case PF_TPI:
    snprintf(buf, len, "%d", (int)value);
    break;
  case PF_THOU:
    snprintf(buf, len, "%d", (int)((value * 1000.0f) + 0.5f));
    break;
  case PF_PERCENT:
    snprintf(buf, len, "%d", (int)((value * 100.0f) + 0.5f));
    break;
  case PF_MM:
  default:
    snprintf(buf, len, "%.2f", (double)value);
    break;
  }
}

// The list the pitch ticker is showing, and the index within it. `jogSpeed`
// forces the jog list whatever the feed mode is -- the JOG SPEED widget is
// reachable from every mode (section 3: OK at rest opens it), which is the one
// case where the list on screen is not the one the feed mode implies.
//
// The index rule lives here rather than in currentPitchTable() so that a
// PitchTable stays a pure description of a LIST, which is what lets the overlay
// ticker ask it for index-1 and index+1.
static PitchTable tickerTable(GlobalState* state, bool jogSpeed, int& index) {
  const GlobalFeedMode mode = state->getFeedMode();
  if (jogSpeed || mode == FM_JOG) {
    index = state->getJogIndex();
    return jogSpeedTable();
  }
  index = state->getFeedSelect();
  return currentPitchTable(mode, state->getUnitMode());
}

// Where the carriage sits between the two stops, 0..1 across the span. Returns
// false when there is no span to measure against: with fewer than two stops
// there is no scale, and parking a marker anywhere would be a made-up reading.
// Position increases to the RIGHT (LeadscrewDirection::RIGHT = 1), so a
// non-positive span means the stops are the wrong way round and is treated the
// same way. Shared by band 4 and the STOPS overlay so the two bars can never
// disagree about where the carriage is.
static bool carriageFraction(Leadscrew* leadscrew, bool leftSet, bool rightSet,
                             float& fraction) {
  if (!leftSet || !rightSet) {
    return false;
  }
  const float lo = leadscrew->getStopPositionMM(LeadscrewStopPosition::LEFT);
  const float hi = leadscrew->getStopPositionMM(LeadscrewStopPosition::RIGHT);
  const float span = hi - lo;
  if (span <= 0.0f) {
    return false;
  }
  float f = (leadscrew->getPositionMM() - lo) / span;
  if (f < 0.0f) {
    f = 0.0f;
  } else if (f > 1.0f) {
    f = 1.0f;
  }
  fraction = f;
  return true;
}

// --- Overlay strings ---------------------------------------------------------
// The hint row names what the arrows do on the left and what OK does on the
// right (section 4). All of these are static literals chosen by a small enum,
// NOT cached through m_textCache -- see the TextSlot comment in the header.
//
// STOPS is the asymmetric one and its hint has to say so: a click SETS, only a
// HOLD clears (setting a stop is cheap to undo, clearing one loses a position
// that may have taken time to find). And UiState refuses every stop edit while
// the carriage is under power (uistate.cpp, UiFocus::Stops), so when that
// inhibit is live the hint must say THAT instead -- offering a gesture the
// machine will silently ignore is the one thing this row must not do.
//
// The MENU variants follow the same rule. A tile whose action is refused right
// now renders dim AND says why, and its right-hand hint drops "OK open" rather
// than promising a keypress that ButtonPad::activateMenuTile() will discard.
enum OverlayHint {
  OH_NONE, OH_PITCH, OH_SPEED, OH_MODE, OH_STOPS, OH_STOPS_LOCKED,
  // The clear-both hold is running (stopsConfirmPermille() > 0): the one thing
  // the row must say is the escape route -- releasing the key is what cancels,
  // and nothing else on the panel says so. Plain dim text, NOT a chip: the
  // colourFault bar above it is already shouting, and two red elements would
  // compete. The right half goes blank with it (OK mid-hold just cancels the
  // hold, which "release to cancel" already covers better).
  OH_STOPS_CONFIRM,
  OH_MENU, OH_MENU_MOVING, OH_MENU_FEED,
  // The DRO datum picker. LOCKED mirrors OH_STOPS_LOCKED and is computed from
  // the SAME motion predicate: UiState refuses DroDatumLeft/Right while the
  // carriage is under power (uistate.cpp, UiFocus::DroDatum), so the row must
  // say that rather than advertise dead arrows. OK still dismisses, so the
  // right half keeps "OK done" (the default arm below), exactly as
  // OH_STOPS_LOCKED does.
  OH_DATUM, OH_DATUM_LOCKED
};
// LV_SYMBOL_* are the FontAwesome codepoints carried by every built-in
// Montserrat face (the same ones drawStateBar() uses), so no extra font is
// pulled in by these.
#define OVERLAY_ARROWS LV_SYMBOL_LEFT LV_SYMBOL_RIGHT
static const char* overlayHintText(OverlayHint hint) {
  switch (hint) {
  case OH_PITCH:        return OVERLAY_ARROWS " pitch";
  case OH_SPEED:        return OVERLAY_ARROWS " speed";
  case OH_MODE:         return OVERLAY_ARROWS " mode";
  case OH_STOPS:        return OVERLAY_ARROWS " set, hold to clear";
  case OH_STOPS_LOCKED: return "moving - stops locked";
  // 132px -- inside the TEXT14_HINT_W bound (171, "moving - datum locked").
  case OH_STOPS_CONFIRM: return "release to cancel";
  case OH_MENU:         return OVERLAY_ARROWS " move";
  // Both measured against TEXT14_HINT_W (161, "moving - stops locked"), which
  // is still the widest string this row can hold: 156 and 145 respectively.
  case OH_MENU_MOVING:  return "stop the carriage first";
  case OH_MENU_FEED:    return "needs thread mode";
  case OH_DATUM:        return OVERLAY_ARROWS " pick";
  // Now the widest string this row can hold: 171px, the TEXT14_HINT_W bound.
  case OH_DATUM_LOCKED: return "moving - datum locked";
  case OH_NONE:
  default:              return "";
  }
}

// The right-hand half of the hint row, on the same variant cache. It used to be
// a single literal set once in init(); the menu needs "OK open" instead of
// "OK done", and a blocked tile needs neither, so it is variant-driven now.
static const char* overlayHintOkText(OverlayHint hint) {
  switch (hint) {
  case OH_MENU:         return "OK open";
  // A blocked tile: OK does nothing, so the row must not offer it. Blank
  // during the clear-both hold too -- the only key that matters is the one
  // being held, and the left half already says what releasing it does.
  case OH_MENU_MOVING:
  case OH_MENU_FEED:
  case OH_STOPS_CONFIRM:
  case OH_NONE:         return "";
  default:              return "OK done";
  }
}

// The nine menu tiles, in MenuTile order (docs/ux-redesign.md section 6). The
// ORDER is defined once, in the MenuTile enum in the header, and shared with
// src/buttonpad.cpp; this is only the rendering of it.
//
// Two constraints on any name added or edited here:
//   * under 20 bytes, or setLabelText()'s cache truncates it, never compares
//     equal again, and repaints the label at 10 Hz forever (see the TextSlot
//     comment in the header);
//   * no single WORD wider than TEXT14_MENU_WORD_W, the measured width the
//     carousel's tiles are asserted against. Names WRAP on spaces inside their
//     tile (at most two lines), so a long name is fine but a long word is not
//     -- LVGL letter-breaks an overwide word mid-glyph run, so re-measure and
//     update that constant.
static const char* menuTileName(int tile) {
  switch (tile) {
  case MENU_UNITS:            return "Units";
  case MENU_THEME:            return "Theme";
  case MENU_DRO_DATUM:        return "DRO datum";
  case MENU_JOG_SPEED:        return "Jog speed";
  case MENU_SYNC:             return "Sync";
  case MENU_SOFTWARE_UPDATE:  return "Software update";
  case MENU_WIFI_SETUP:       return "Wi-Fi setup";
  case MENU_DIAGNOSTICS:      return "Diagnostics";
  case MENU_ABOUT:            return "About";
  // Two words, neither wider than TEXT14_MENU_WORD_W ("Diagnostics" at 85px is
  // still the widest), and 13 bytes - inside the cache cap above.
  case MENU_DEBUG_CAPTURE:    return "Debug capture";
  // Out of range: blank, the same answer formatPitch() gives for an index off
  // the end of a pitch table. The carousel asks for index-1 and index+1 on
  // purpose, and a blank neighbour is the correct rendering at the two ends.
  default:                    return "";
  }
}

Display::Display(Spindle* spindle, Leadscrew* leadscrew, const UiState* ui) {
  this->m_spindle = spindle;
  this->m_leadscrew = leadscrew;
  this->m_ui = ui;
  this->m_globalState = GlobalState::getInstance();
  // Theme is picked once here, not re-read from config on every init() rebuild
  // (Display::update() calls init() whenever getDisplayReset() fires) -- if it
  // were re-read every time, a future runtime setTheme() call would just get
  // clobbered back to the config default on the very next rebuild it triggers.
  // Any value other than THEME_LIGHT falls back to dark, mirroring the same
  // safe-fallback pattern latheconfig.cpp uses for droDatum.
  uint8_t theme = (leadscrew != nullptr) ? leadscrew->getConfig()->theme() : THEME_DARK;
  this->m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
  // Same story as the theme one line up, and for the same reason: seeded from
  // config ONCE here, then owned at runtime by setDroDatum(). Re-reading it in
  // drawTravel() would clobber a menu change on the very next tick.
  this->m_droDatum = (leadscrew != nullptr) ? leadscrew->getConfig()->droDatum()
                                            : DroDatumPreference::Left;
  // Owned by initDisplay(), which is the only writer and runs before any read
  // of either -- but CLAUDE.md's rule is every member, and these two are the
  // ones the class was missing (Display is `new`ed, so they are heap garbage
  // until then, and a stray read would be a wild pointer rather than a crash).
  this->disp = nullptr;
  this->draw_buf = nullptr;
  // Host-injected About-screen network state; the device path reads WiFi
  // directly and never looks at these, but they are members and Display is
  // `new`ed, so they must not start as heap garbage.
  this->m_aboutIp = IPAddress();
  this->m_aboutConnected = false;
  resetObjectTree();
}

Display::Display() {
  // Used only for the Wi-Fi setup-mode screen (main.cpp runWifiSettings()),
  // via showWifi()/showConnected() -- init() (the themed dashboard built
  // below) is never called on this path, so m_spindle/m_leadscrew are
  // genuinely unused here. Per CLAUDE.md ("Constructors must initialise all
  // members" -- this object is heap-allocated with `new`, and the heap is not
  // zero-initialised) they must still be set explicitly rather than left
  // holding garbage, hence the explicit nullptr rather than omitting them.
  this->m_spindle = nullptr;
  this->m_leadscrew = nullptr;
  // No ButtonPad exists on the setup path (main.cpp never builds one), so there
  // is no focus to render. drawOverlay() is not reached from showWifi() /
  // showConnected() at all, but every overlay path still tests this for nullptr
  // rather than assuming that stays true.
  this->m_ui = nullptr;
  this->m_globalState = GlobalState::getInstance();
  this->m_palette = &PALETTE_DARK;  // no config available on this path -- default dark.
  this->m_droDatum = DroDatumPreference::Left;  // ditto; nothing on this path
                                                // draws the travel band anyway.
  this->disp = nullptr;             // see the other constructor.
  this->draw_buf = nullptr;
  this->m_aboutIp = IPAddress();    // see the other constructor.
  this->m_aboutConnected = false;
  resetObjectTree();
}

// Every lv_obj_t* and every redraw-suppression cache, in one place, called from
// BOTH constructors and again at the top of init(). Two reasons it exists:
//   * CLAUDE.md: Display is heap-allocated, so nothing here is implicitly zero.
//     The Wi-Fi-only constructor never builds the dashboard at all, and its
//     object pointers must still be nullptr rather than garbage.
//   * init() is a full REBUILD (lv_obj_clean drops every object), so the caches
//     must be cleared with it -- otherwise a cache entry left over from the old
//     tree would suppress the first push into the new, blank, label.
void Display::resetObjectTree() {
  modeLabel = nullptr;
  unitLabel = nullptr;
  syncLabel = nullptr;
  rpmLabel = nullptr;
  rpmUnitLabel = nullptr;
  pitchLabel = nullptr;
  pitchUnitLabel = nullptr;
  feedSymbolObj = nullptr;
  for (int i = 0; i < PIP_MAX; i++) {
    pitchPips[i] = nullptr;
  }
  travelTrack = nullptr;
  travelLeftMark = nullptr;
  travelRightMark = nullptr;
  travelCarriage = nullptr;
  travelLeftLabel = nullptr;
  travelRightLabel = nullptr;
  travelPosLabel = nullptr;
  travelPosUnit = nullptr;
  stateLabel = nullptr;
  for (int i = 0; i < 4; i++) {
    bandRule[i] = nullptr;
  }
  overlayPanel = nullptr;
  overlayTitle = nullptr;
  overlayHintLeft = nullptr;
  overlayHintRight = nullptr;
  overlayTickerGroup = nullptr;
  overlayTickerPrev = nullptr;
  overlayTickerValue = nullptr;
  overlayTickerNext = nullptr;
  overlayTickerUnit = nullptr;
  overlayModeGroup = nullptr;
  for (int i = 0; i < 3; i++) {
    overlayModeTile[i] = nullptr;
    overlayModeTileLabel[i] = nullptr;
  }
  overlayStopsGroup = nullptr;
  overlayStopsTrack = nullptr;
  overlayStopsLeftMark = nullptr;
  overlayStopsRightMark = nullptr;
  overlayStopsCarriage = nullptr;
  overlayStopsLeftLabel = nullptr;
  overlayStopsRightLabel = nullptr;
  overlayStopsPosLabel = nullptr;
  overlayStopsConfirmTrack = nullptr;
  overlayStopsConfirmFill = nullptr;
  overlayStopsConfirmLabel = nullptr;
  overlayMenuGroup = nullptr;
  overlayMenuPos = nullptr;
  for (int i = 0; i < 3; i++) {
    overlayMenuTile[i] = nullptr;
    overlayMenuTileLabel[i] = nullptr;
  }
  overlayDatumGroup = nullptr;
  for (int i = 0; i < 2; i++) {
    overlayDatumTile[i] = nullptr;
    overlayDatumTileLabel[i] = nullptr;
    overlayDatumBar[i] = nullptr;
    overlayDatumZero[i] = nullptr;
  }
  diagPanel = nullptr;
  diagTitle = nullptr;
  diagErrValue = nullptr;
  diagErrPulses = nullptr;
  diagErrMarker = nullptr;
  diagSpindleValue = nullptr;
  diagCarriageValue = nullptr;
  diagExpectValue = nullptr;
  diagSyncChip = nullptr;
  diagAnchorValue = nullptr;
  aboutPanel = nullptr;
  aboutIpValue = nullptr;
  aboutUptimeValue = nullptr;
  updateSlider = nullptr;
  updateLabel = nullptr;

  for (int i = 0; i < TS_COUNT; i++) {
    m_textCache[i][0] = '\0';
  }
  m_lastFeedSrc = nullptr;
  m_lastSyncState = -1;
  m_lastMotionMode = -1;
  m_lastDatumSource = -1;
  m_lastRpmNegative = false;
  m_lastCarriageX = -1;
  m_lastCarriageShown = false;
  m_lastLeftStopSet = false;
  m_lastRightStopSet = false;
  // -1, not UiFocus::Jog: the first drawOverlay() after a rebuild must run its
  // focus-changed branch even if focus has been sitting on Jog the whole time,
  // because that branch is what puts the freshly-built panel into the hidden
  // state and selects a content group.
  m_lastFocus = -1;
  m_lastHintVariant = (int)OH_NONE;
  m_lastModeTile = -1;
  m_lastOverlayLeftStopSet = false;
  m_lastOverlayRightStopSet = false;
  m_lastOverlayCarriageX = -1;
  m_lastOverlayCarriageShown = false;
  // FALSE to match init(), which builds the confirm bar hidden; -1 so the
  // first live draw always pushes a fill width.
  m_lastStopsConfirmShown = false;
  m_lastStopsConfirmFillW = -1;
  // -1, not MTB_NONE: the first drawOverlayMenu() after a rebuild must run its
  // restyle branch even when nothing is blocked, because that branch is what
  // paints the freshly-built card in the accent for the first time.
  m_lastMenuBlock = -1;
  m_lastMenuPrevBlock = -1;
  m_lastMenuNextBlock = -1;
  // TRUE, not false: init() builds the neighbour tiles visible, and these gate
  // the hidden flag, so they must describe the tree as built (see the header).
  m_lastMenuPrevShown = true;
  m_lastMenuNextShown = true;
  // -1 = "not laid out / not styled yet" so the first drawPitch() after a
  // rebuild both reflows and restyles the freshly-built (all-ghost) pip row.
  m_lastPipCount = -1;
  m_lastPipIndex = -1;
  m_lastDatumTile = -1;
  m_lastDiagErrX = -1;
  m_lastDiagErrPegged = false;  // init() builds the marker textPrimary
  m_lastDiagSyncState = -1;
  m_lastAboutConnected = -1;
  // NOTE m_palette and m_droDatum are deliberately NOT reset here. Both are
  // RUNTIME settings owned by setTheme()/setDroDatum(), and init() calls this
  // on every rebuild -- including the rebuild setTheme() itself requests, which
  // would then immediately undo the change that triggered it.
}

bool Display::setLabelText(lv_obj_t* label, int slot, const char* text) {
  if (label == nullptr) {
    return false;
  }
  if (strcmp(m_textCache[slot], text) == 0) {
    return false;
  }
  snprintf(m_textCache[slot], TEXT_SLOT_LEN, "%s", text);
  lv_label_set_text(label, text);
  return true;
}

// Runtime theme switch, driven by the "Theme" menu tile. Re-points m_palette
// and asks Display::update() to rebuild the whole screen from scratch next tick
// via the existing getDisplayReset()/init() path (see Display::update() below),
// the same mechanism already used for the OTA <-> normal screen swap, so no new
// plumbing is needed. Every colour on the screen comes from *m_palette, so the
// full rebuild is what makes the swap total rather than partial.
void Display::setTheme(uint8_t theme) {
  m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
  m_globalState->setDisplayReset();
}

// Runtime DRO datum switch, driven by the "DRO datum" menu tile. No rebuild:
// drawTravel() reads m_droDatum every tick and its own caches (m_lastDatumSource
// and the TS_TRAVEL_* text slots) notice the change on the next pass, so the
// datum end, the two stop readouts and the live position all move together.
void Display::setDroDatum(DroDatumPreference datum) {
  m_droDatum = datum;
}

void Display::initvars() {

}

// Append src to a Wi-Fi QR payload, escaping the characters that are special in
// the "WIFI:" URI scheme (\ ; , : ") with a leading backslash.
static void appendWifiQrEscaped(String& out, const char* src) {
  for (const char* p = src; *p != '\0'; ++p) {
    char c = *p;
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
      out += '\\';
    }
    out += c;
  }
}

// The boot splash. See the SPLASH_* constants for the layout and its asserts.
//
// Deliberately drawn on the PALETTE the display was constructed with rather
// than pinned to dark like the Wi-Fi screens: by the time this runs the stored
// config has been read and validated (main.cpp calls it on the normal boot
// path, after the Display is constructed with a real theme), so a light-theme
// machine should not flash a dark screen for two seconds before its dashboard
// arrives. On the AP-setup path there IS no valid theme -- and no splash: that
// path wants the join credentials on screen as fast as possible.
//
// The mark is recoloured to the ACCENT, the one colour in either palette that
// belongs to the project rather than to a machine state. Nothing on this screen
// is a machine state, so none of the state colours may appear here.
void Display::showSplash() {
  initDisplay();

  lv_obj_set_style_bg_color(lv_screen_active(), m_palette->background, 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  lv_obj_t* mark = lv_image_create(lv_screen_active());
  lv_image_set_src(mark, &halfNutLogo);
  lv_obj_set_pos(mark, SPLASH_LOGO_X, SPLASH_LOGO_Y);
  // A8 mask: it has alpha but no colour of its own, exactly like the mode
  // glyphs, so the recolour IS the colour.
  lv_obj_set_style_image_recolor(mark, m_palette->accent, 0);
  lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);

  lv_obj_t* name = createLabel(lv_screen_active(), &lv_font_montserrat_36,
                               m_palette->textPrimary, 0, SPLASH_NAME_Y);
  fixLabelBox(name, SCREEN_W, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(name, "HalfNut ELS");

  lv_obj_t* tag = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                              m_palette->textDim, 0, SPLASH_TAG_Y);
  fixLabelBox(tag, SCREEN_W, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(tag, "ELECTRONIC LEADSCREW");

  lv_obj_t* version = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                  m_palette->textDim, 0, SPLASH_VERSION_Y);
  fixLabelBox(version, SCREEN_W, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(version, FIRMWARE_VERSION);

  lv_timer_handler();
}

void Display::showWifi(const char* ssid, const char* password, IPAddress ip) {
  initDisplay();

  // These are the first screens anyone sees on a new device, and they used to
  // sit on LVGL's stock light-grey ground looking like a different product.
  // They are on the dark palette now (m_palette is pinned to PALETTE_DARK by
  // the no-arg constructor -- there is no valid stored config to read a theme
  // from on this path, and m_leadscrew is null, so nothing here may look at
  // LatheConfig). See the WIFI_* constants for the layout and its assertions.
  lv_obj_set_style_bg_color(lv_screen_active(), m_palette->background, 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  // Left column: credentials as text (fallback if the QR can't be scanned).
  // Same grammar as everywhere else: dim 14 label over primary 26 value.
  lv_obj_t* ssidLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                    m_palette->textDim, WIFI_LABEL_X,
                                    WIFI_SSID_LABEL_Y);
  lv_label_set_text(ssidLabel, "Wifi SSID");
  lv_obj_t* ssidText = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                                   m_palette->textPrimary, WIFI_LABEL_X,
                                   WIFI_SSID_VALUE_Y);
  fixLabelBox(ssidText, WIFI_TEXT_W, LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(ssidText, ssid);

  lv_obj_t* passwordLabel = createLabel(lv_screen_active(),
                                        &lv_font_montserrat_14,
                                        m_palette->textDim, WIFI_LABEL_X,
                                        WIFI_PASS_LABEL_Y);
  lv_label_set_text(passwordLabel, "Password");
  lv_obj_t* passwordText = createLabel(lv_screen_active(),
                                       &lv_font_montserrat_26,
                                       m_palette->textPrimary, WIFI_LABEL_X,
                                       WIFI_PASS_VALUE_Y);
  fixLabelBox(passwordText, WIFI_TEXT_W, LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(passwordText, password);

  lv_obj_t* ipLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                  m_palette->textDim, WIFI_LABEL_X,
                                  WIFI_IP_LABEL_Y);
  lv_label_set_text(ipLabel, "IP Address");
  lv_obj_t* ipText = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                                 m_palette->textPrimary, WIFI_LABEL_X,
                                 WIFI_IP_VALUE_Y);
  fixLabelBox(ipText, WIFI_TEXT_W, LV_TEXT_ALIGN_LEFT);
  lv_label_set_text(ipText, ip.toString().c_str());

  // Right side: a Wi-Fi join QR code. Scanning it on a phone connects straight
  // to the setup AP (the captive portal then opens the config page). The code
  // is dark-on-light ON PURPOSE and stays that way whatever happens to the
  // palette -- inverted QR fails on some scanners -- so on the dark ground it
  // sits on a deliberate white card whose padding is the quiet zone. Black and
  // white here are functional, not styling; everything else on the screen is
  // palette ink.
  lv_obj_t* scanLabel = createLabel(
    lv_screen_active(), &lv_font_montserrat_14, m_palette->textDim,
    WIFI_CARD_X + ((WIFI_CARD_W - TEXT14_WIFI_SCAN_W) / 2), WIFI_SCAN_Y);
  lv_label_set_text(scanLabel, "Scan to join");

  String qrPayload = "WIFI:S:";
  appendWifiQrEscaped(qrPayload, ssid);
  qrPayload += ";T:WPA;P:";
  appendWifiQrEscaped(qrPayload, password);
  qrPayload += ";;";

  lv_obj_t* wifiQr = lv_qrcode_create(lv_screen_active());
  lv_qrcode_set_size(wifiQr, WIFI_QR_SIZE);
  lv_qrcode_set_dark_color(wifiQr, lv_color_black());
  lv_qrcode_set_light_color(wifiQr, lv_color_white());
  // The white card: bg + padding around the canvas. Radius 4 (RADIUS_TRACK,
  // one of the three sanctioned radii) so it reads as a placed card rather
  // than a rendering hole in the dark ground.
  lv_obj_set_style_bg_color(wifiQr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(wifiQr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(wifiQr, 0, 0);
  lv_obj_set_style_radius(wifiQr, RADIUS_TRACK, 0);
  lv_obj_set_style_pad_all(wifiQr, WIFI_QR_PAD, 0);
  lv_qrcode_update(wifiQr, qrPayload.c_str(), qrPayload.length());
  lv_obj_set_pos(wifiQr, WIFI_CARD_X, WIFI_CARD_Y);

  lv_timer_handler();

}

// Shown once a device has joined the setup AP. Deliberately NO QR: on phones the
// OS routes the plain browser over cellular while the AP is "captive", so a
// browser QR would mislead users into a route that won't work. Phones should use
// the OS "Sign in to network" prompt; the IP is shown for computers on the AP.
void Display::showConnected(IPAddress ip) {
  // Replace the join screen (LVGL is already initialised by showWifi()).
  lv_obj_clean(lv_screen_active());

  // Same dark palette as showWifi() -- and set again here rather than relying
  // on the style showWifi() left on the screen object, so this screen renders
  // correctly even if the call order ever changes. m_leadscrew is null on this
  // path; nothing here may read config.
  lv_obj_set_style_bg_color(lv_screen_active(), m_palette->background, 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  // colourRun, not textPrimary: this is the one genuinely good-news headline
  // in the whole UI, and the run green reads ~8.8:1 on the dark ground. (The
  // "no coloured text" chip rule exists for the LIGHT palette's white ground;
  // this screen is always dark -- see the WIFI_* constants.)
  lv_obj_t* title = createLabel(lv_screen_active(), &lv_font_montserrat_36,
                                m_palette->colourRun, WIFI_LABEL_X,
                                CONN_TITLE_Y);
  lv_label_set_text(title, "Connected!");

  lv_obj_t* msg = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                              m_palette->textPrimary, WIFI_LABEL_X,
                              CONN_MSG_Y);
  // CONN_MSG_LINES lines -- keep the constant in step with this string.
  lv_label_set_text(msg,
    "A device joined the setup network.\n\n"
    "On your phone, tap the\n"
    "\"Sign in to network\" prompt\n"
    "to open the configuration page.");

  lv_obj_t* ipLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                  m_palette->textDim, WIFI_LABEL_X,
                                  CONN_IP_LABEL_Y);
  lv_label_set_text(ipLabel, "On a computer, browse to:");

  lv_obj_t* ipText = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                                 m_palette->textPrimary, WIFI_LABEL_X,
                                 CONN_IP_VALUE_Y);
  lv_label_set_text(ipText, ip.toString().c_str());

  lv_timer_handler();

}


// One-time LVGL + panel bring-up. ALL of it is behind `initialised`, not just
// the malloc: init() is documented (and used) as a full-rebuild path, so this
// is reachable more than once. Re-running lv_init() and lv_tft_espi_create()
// on a rebuild would re-initialise LVGL underneath the live object tree and
// leak a whole lv_display_t plus its driver state every time. Nothing calls it
// twice today -- getDisplayReset() has exactly one setter, the currently
// unwired setTheme() -- so this is a no-op now and a prerequisite for wiring
// the theme menu (FS-I3), which is what makes setTheme() live.
void Display::initDisplay() {
  if (initialised) {
    return;
  }
  initialised = true;

  draw_buf = (uint32_t*)malloc(DRAW_BUF_SIZE);
  lv_init();
  lv_tick_set_cb(my_tick);
  disp = lv_tft_espi_create(TFT_WIDTH, TFT_HEIGHT, draw_buf, DRAW_BUF_SIZE);
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
}

void Display::initialiseOta() {
  initOta = true;
  lv_obj_clean(lv_screen_active());
  // The OTA screen replaced everything init() built, so those pointers are now
  // dangling; drop them (and the caches) before anything can push into them.
  resetObjectTree();

  // This screen is reachable while m_palette points at either theme, and it
  // shares the screen object init() painted -- so its ink and bar must come
  // from the palette too. LVGL's label default is BLACK, which on the dark
  // palette's near-black ground would render the OTA status invisible.
  lv_obj_set_style_bg_color(lv_screen_active(), m_palette->background, 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  updateLabel = lv_label_create(lv_screen_active());
  lv_label_set_text(updateLabel, "Updating...");
  lv_obj_set_style_text_font(updateLabel, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_color(updateLabel, m_palette->textPrimary, 0);
  lv_obj_align(updateLabel, LV_ALIGN_CENTER, 0, 0);

  updateSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(updateSlider, 280, 10);
  lv_obj_set_pos(updateSlider, 20, 150);

  lv_obj_set_style_opa(updateSlider, LV_OPA_0, LV_PART_KNOB);
  // Track dim, progress in the run colour, both explicitly opaque (the slider
  // theme's translucent main part is the same trap the pitch ticker hit).
  lv_obj_set_style_bg_color(updateSlider, m_palette->colourDisabled, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(updateSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(updateSlider, m_palette->colourRun, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(updateSlider, LV_OPA_COVER, LV_PART_INDICATOR);

  lv_slider_set_range(updateSlider, 0, 100);
  lv_obj_set_style_pad_all(updateSlider, 0, 0);
}

// Builds the whole main screen ONCE. Nothing here may be repeated per tick: the
// draw*() methods below only push values into the objects created here.
void Display::init() {

  initDisplay();

  // init() is also the rebuild path (Display::update() calls it whenever
  // getDisplayReset() fires, e.g. after setTheme()), so start from a clean
  // screen and clean state rather than stacking a second tree on top of the
  // first.
  lv_obj_clean(lv_screen_active());
  resetObjectTree();

  lv_obj_set_style_bg_color(lv_screen_active(), m_palette->background, 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  // --- band rules -----------------------------------------------------------
  const int ruleY[4] = { BAND_STATUS_BOTTOM, BAND_PITCH_BOTTOM,
                         BAND_TICKER_BOTTOM, BAND_TRAVEL_BOTTOM };
  for (int i = 0; i < 4; i++) {
    bandRule[i] = createRect(lv_screen_active(), 0, ruleY[i], SCREEN_W, 1,
                             m_palette->colourDisabled, 0);
  }

  // --- band 1: status bar ---------------------------------------------------
  modeLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                          m_palette->textPrimary, STATUS_MODE_X, STATUS_CHIP_Y);
  unitLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                          m_palette->textDim, STATUS_UNIT_X, STATUS_CHIP_Y);
  // Static text; drawStatusBar() tracks GlobalThreadSyncState by toggling the
  // chip: synced = colourRun fill + chipInk text, unsynced = no fill + textDim
  // text. A filled chip, not coloured text, because colourRun TEXT on the light
  // palette's white ground is 2.2:1 -- invisible -- while chipInk on the fill
  // is ~8.5:1 in both palettes. The label is positioned pad-left/pad-up of the
  // ink coordinates so the TEXT sits at (STATUS_SYNC_X, STATUS_CHIP_Y) whether
  // or not the fill is showing; the fill colour is set once here and only
  // bg_opa/text colour toggle at runtime.
  syncLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                          m_palette->textDim, STATUS_SYNC_X - SYNC_CHIP_PAD_H,
                          STATUS_CHIP_Y - SYNC_CHIP_PAD_V);
  lv_label_set_text(syncLabel, "SYNC");
  lv_obj_set_style_pad_hor(syncLabel, SYNC_CHIP_PAD_H, 0);
  lv_obj_set_style_pad_ver(syncLabel, SYNC_CHIP_PAD_V, 0);
  lv_obj_set_style_radius(syncLabel, RADIUS_TRACK, 0);
  lv_obj_set_style_bg_color(syncLabel, m_palette->colourRun, 0);
  lv_obj_set_style_bg_opa(syncLabel, LV_OPA_TRANSP, 0);

  rpmLabel = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                         m_palette->textPrimary, STATUS_RPM_VALUE_X,
                         STATUS_RPM_VALUE_Y);
  fixLabelBox(rpmLabel, STATUS_RPM_VALUE_W, LV_TEXT_ALIGN_RIGHT);
  rpmUnitLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                             m_palette->textDim, STATUS_RPM_UNIT_X,
                             STATUS_RPM_UNIT_Y);
  lv_label_set_text(rpmUnitLabel, "RPM");

  // --- band 2: primary readout ---------------------------------------------
  pitchLabel = createLabel(lv_screen_active(), &lv_font_montserrat_48,
                           m_palette->textPrimary, PITCH_VALUE_X, PITCH_VALUE_Y);
  // Deliberately auto-width (LV_SIZE_CONTENT): the unit is positioned relative
  // to it in drawPitch(), so the pair stays tight for "16 TPI" and "1.25 mm"
  // alike instead of leaving a hole after short values.
  pitchUnitLabel = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                               m_palette->textDim, PITCH_VALUE_X, PITCH_VALUE_Y);

  feedSymbolObj = lv_image_create(lv_screen_active());
  lv_obj_set_pos(feedSymbolObj, MODE_GLYPH_X, MODE_GLYPH_Y);
  lv_image_set_src(feedSymbolObj, &threadSymbol);
  m_lastFeedSrc = &threadSymbol;
  // The icons are A8 (alpha-only; colour comes from the image's recolor style).
  // LVGL's A8 blend path applies draw_dsc->recolor unconditionally (unlike other
  // formats, which gate on recolor_opa > LV_OPA_MIN), and the style default for
  // LV_STYLE_IMAGE_RECOLOR is black -- so without an explicit recolor here it
  // renders black regardless of theme. Set per object, not per source, because
  // drawMode() swaps lv_image_set_src at runtime on this same object.
  //
  // textDim, NOT textPrimary: at 128x64 of solid ink the glyph out-shouted the
  // pitch value, which is the number the operator actually reads -- and the
  // status bar already names the mode in text, so the glyph is a secondary
  // echo, not the primary carrier. Dimmed it still reads clearly (5.2:1 dark /
  // 4.9:1 light) but the 48px pitch value now dominates band 2.
  lv_obj_set_style_image_recolor(feedSymbolObj, m_palette->textDim, 0);
  lv_obj_set_style_image_recolor_opa(feedSymbolObj, LV_OPA_COVER, 0);

  // --- band 3: the pitch pip row --------------------------------------------
  // One rect per possible list entry, all built here as short "other" ticks on
  // the common baseline; drawPitch() positions the first `count` across the
  // row (the reflow, on a count change only) and promotes the current pip and
  // its neighbours (the restyle, on an index change only). NOT a slider: the
  // list is discrete, and a continuous track read as a twin of the travel bar
  // directly below. Radius 0 -- these are ruler ticks, like the stop marks.
  {
    // The five tables the row can show must all fit the pips built here --
    // checked against the REAL arrays, so growing a table without growing
    // PIP_MAX fails the build instead of silently hiding the new entries.
    static_assert((int)ARRAY_SIZE(threadPitchMetric) <= PIP_MAX, "pitch table longer than the pip row");
    static_assert((int)ARRAY_SIZE(feedPitchMetric) <= PIP_MAX, "pitch table longer than the pip row");
    static_assert((int)ARRAY_SIZE(threadPitchImperial) <= PIP_MAX, "pitch table longer than the pip row");
    static_assert((int)ARRAY_SIZE(feedPitchImperial) <= PIP_MAX, "pitch table longer than the pip row");
    static_assert((int)ARRAY_SIZE(jogSpeeds) <= PIP_MAX, "jog table longer than the pip row");
  }
  for (int i = 0; i < PIP_MAX; i++) {
    pitchPips[i] = createRect(lv_screen_active(), PIP_ROW_X,
                              PIP_BASE_Y - PIP_H_OTHER, PIP_W, PIP_H_OTHER,
                              m_palette->colourDisabled, 0);
    lv_obj_add_flag(pitchPips[i], LV_OBJ_FLAG_HIDDEN);
  }

  // --- band 4: carriage travel ---------------------------------------------
  travelTrack = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_TRACK_Y,
                           TRAVEL_TRACK_W, TRAVEL_TRACK_H,
                           m_palette->colourDisabled, RADIUS_TRACK);
  // Markers and the carriage are siblings of the track, not children of it: a
  // child would be clipped to the 10px-high track, and these are 16 tall.
  //
  // Both end markers are ALWAYS shown; drawTravel() colours them colourRun
  // (set) or colourDisabled (a "ghost" tick, unset) rather than hiding them.
  // Hiding an unset end left a lone green tick at the far end of an empty
  // grey track, which reads as an empty progress bar; with both endpoints
  // always drawn the band reads as a span whose ends are defined or not --
  // which is what it is. Created as ghosts to match the m_last*StopSet caches,
  // which resetObjectTree() has just cleared to false.
  travelLeftMark = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_MARK_Y,
                              TRAVEL_MARK_W, TRAVEL_MARK_H,
                              m_palette->colourDisabled, 0);
  travelRightMark = createRect(lv_screen_active(),
                               TRAVEL_TRACK_X + TRAVEL_TRACK_W - TRAVEL_MARK_W,
                               TRAVEL_MARK_Y, TRAVEL_MARK_W, TRAVEL_MARK_H,
                               m_palette->colourDisabled, 0);
  travelCarriage = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_MARK_Y,
                              TRAVEL_CARRIAGE_W, TRAVEL_MARK_H,
                              m_palette->textPrimary, RADIUS_TRACK);
  lv_obj_add_flag(travelCarriage, LV_OBJ_FLAG_HIDDEN);

  travelLeftLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                m_palette->textDim, TRAVEL_LEFT_X, TRAVEL_LABEL_Y);
  // Boxed like its three neighbours: without this the label is auto-width and
  // grows rightward into travelPosLabel's box as the value gets longer, which
  // is the one place on this screen where two readouts could overlap.
  fixLabelBox(travelLeftLabel, TRAVEL_LEFT_W, LV_TEXT_ALIGN_LEFT);
  travelRightLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                 m_palette->textDim, TRAVEL_RIGHT_X, TRAVEL_LABEL_Y);
  fixLabelBox(travelRightLabel, TRAVEL_RIGHT_W, LV_TEXT_ALIGN_RIGHT);
  travelPosLabel = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                               m_palette->textPrimary, TRAVEL_POS_X, TRAVEL_VALUE_Y);
  fixLabelBox(travelPosLabel, TRAVEL_POS_W, LV_TEXT_ALIGN_RIGHT);
  travelPosUnit = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                              m_palette->textDim, TRAVEL_POS_UNIT_X, TRAVEL_LABEL_Y);

  // --- band 5: state chip ---------------------------------------------------
  // One auto-width Montserrat-26 label carrying a state-coloured chip fill;
  // drawStateBar() pushes the word and the fill colour. A filled chip, not a
  // coloured word, because the state colours as TEXT fail contrast on the
  // light palette's white ground (colourRun is 2.2:1) while chipInk on the
  // fill clears 3.9:1 on the dimmest state (IDLE / colourDisabled) and 5.3:1+
  // on the rest, in both palettes. (The soft-key hint row that shared this
  // band is gone: the physical caps are labelled, so it repeated the keypad.
  // 26, not the 36 it briefly was: the owner judged 36 slightly too big, and
  // the freed height became the air between bands 3 and 4.)
  stateLabel = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                           m_palette->chipInk, STATE_CHIP_X, STATE_CHIP_Y);
  lv_obj_set_style_pad_hor(stateLabel, STATE_CHIP_PAD_H, 0);
  lv_obj_set_style_radius(stateLabel, RADIUS_TRACK, 0);
  lv_obj_set_style_bg_color(stateLabel, m_palette->colourDisabled, 0);
  lv_obj_set_style_bg_opa(stateLabel, LV_OPA_COVER, 0);

  // --- the selector overlay (docs/ux-redesign.md section 4) -----------------
  // Built LAST, so it is the last sibling on the screen and therefore drawn on
  // top of every band above. Built ONCE and left hidden: drawOverlay() only
  // toggles LV_OBJ_FLAG_HIDDEN and pushes values, it never creates or deletes
  // anything, because this whole tree would otherwise be rebuilt ten times a
  // second and every redraw cache would be pointless.
  overlayPanel = createRect(lv_screen_active(), OVERLAY_X, OVERLAY_Y,
                            OVERLAY_W, OVERLAY_H, m_palette->surface,
                            RADIUS_TRACK);
  // Solid fill (from createRect) plus a border in the focus accent: the accent
  // is what says "the arrows drive what is in here" (section 1). The fill has
  // to be opaque -- LV_DRAW_LAYER_SIMPLE_BUF_SIZE is 24 KB and a translucent
  // 304x148 RGB565 panel would need ~90 KB (section 8).
  lv_obj_set_style_border_width(overlayPanel, OVERLAY_BORDER, 0);
  lv_obj_set_style_border_color(overlayPanel, m_palette->accent, 0);
  lv_obj_set_style_border_opa(overlayPanel, LV_OPA_COVER, 0);
  // Explicit, because the content-area arithmetic every child coordinate is
  // written in depends on the border being inset on all four sides.
  lv_obj_set_style_border_side(overlayPanel, LV_BORDER_SIDE_FULL, 0);
  lv_obj_add_flag(overlayPanel, LV_OBJ_FLAG_HIDDEN);

  // Title and hints are shared by all four focuses; only their text changes.
  overlayTitle = createLabel(overlayPanel, &lv_font_montserrat_14,
                             m_palette->textDim, 0, OVERLAY_TITLE_Y);
  fixLabelBox(overlayTitle, OVERLAY_CONTENT_W, LV_TEXT_ALIGN_CENTER);
  // Auto-width, permanently padded, fill colour set once: drawOverlay() only
  // toggles bg_opa (and the ink) when the hint becomes/stops being a refusal,
  // so the chip hugs whatever text it holds (see the OVERLAY_HINT_* comment).
  overlayHintLeft = createLabel(overlayPanel, &lv_font_montserrat_14,
                                m_palette->textDim,
                                OVERLAY_HINT_L_X - OVERLAY_HINT_PAD_H,
                                OVERLAY_HINT_Y - OVERLAY_HINT_PAD_V);
  lv_obj_set_style_pad_hor(overlayHintLeft, OVERLAY_HINT_PAD_H, 0);
  lv_obj_set_style_pad_ver(overlayHintLeft, OVERLAY_HINT_PAD_V, 0);
  lv_obj_set_style_radius(overlayHintLeft, RADIUS_TRACK, 0);
  lv_obj_set_style_bg_color(overlayHintLeft, m_palette->colourCaution, 0);
  lv_obj_set_style_bg_opa(overlayHintLeft, LV_OPA_TRANSP, 0);
  overlayHintRight = createLabel(overlayPanel, &lv_font_montserrat_14,
                                 m_palette->textDim, OVERLAY_HINT_R_X,
                                 OVERLAY_HINT_Y);
  fixLabelBox(overlayHintRight, OVERLAY_HINT_R_W, LV_TEXT_ALIGN_RIGHT);
  // No literal here any more. This used to be a fixed "OK done", but the menu
  // needs "OK open" and a blocked tile needs neither, so the right half is
  // driven by the SAME hint variant as the left (drawOverlay()). Seeding it
  // here would put the label out of step with m_lastHintVariant, which
  // resetObjectTree() has just set to OH_NONE - i.e. "".

  // Group A: the ticker. RATE and JOG SPEED share it -- section 4 gives them
  // the same shape, and the only difference is which table feeds it.
  overlayTickerGroup = createOverlayGroup(overlayPanel);
  overlayTickerPrev = createLabel(overlayTickerGroup, &lv_font_montserrat_26,
                                  m_palette->textDim, OVERLAY_TICK_PREV_X,
                                  OVERLAY_TICK_SIDE_Y);
  fixLabelBox(overlayTickerPrev, OVERLAY_TICK_SIDE_W, LV_TEXT_ALIGN_RIGHT);
  overlayTickerNext = createLabel(overlayTickerGroup, &lv_font_montserrat_26,
                                  m_palette->textDim, OVERLAY_TICK_NEXT_X,
                                  OVERLAY_TICK_SIDE_Y);
  fixLabelBox(overlayTickerNext, OVERLAY_TICK_SIDE_W, LV_TEXT_ALIGN_LEFT);
  overlayTickerValue = createLabel(overlayTickerGroup, &lv_font_montserrat_48,
                                   m_palette->textPrimary,
                                   OVERLAY_TICK_VALUE_X, OVERLAY_TICK_VALUE_Y);
  fixLabelBox(overlayTickerValue, OVERLAY_TICK_VALUE_W, LV_TEXT_ALIGN_CENTER);
  overlayTickerUnit = createLabel(overlayTickerGroup, &lv_font_montserrat_26,
                                  m_palette->textDim, OVERLAY_TICK_VALUE_X,
                                  OVERLAY_TICK_UNIT_Y);
  fixLabelBox(overlayTickerUnit, OVERLAY_TICK_VALUE_W, LV_TEXT_ALIGN_CENTER);

  // Group B: MODE. Tiles first, labels after, so the labels are the later
  // siblings and draw on top of the fills (they are siblings, not children of
  // the tiles, which keeps every coordinate in one space).
  //
  // Unselected tiles are BACKGROUND-filled wells with a 1px colourDisabled
  // border and textDim ink -- NOT colourDisabled slabs with textDim ink, which
  // measured 1.05:1 (light) / 1.3:1 (dark): blank slabs. Ink on background is
  // 5.2:1 / 4.9:1. The border is what keeps a white-on-white tile visible in
  // the light palette (its background and surface are 1.2:1 apart).
  // drawOverlayMode() swaps the selected tile to accent fill + chipInk.
  overlayModeGroup = createOverlayGroup(overlayPanel);
  for (int i = 0; i < 3; i++) {
    overlayModeTile[i] = createRect(
      overlayModeGroup,
      OVERLAY_MODE_X0 + (i * (OVERLAY_MODE_TILE_W + OVERLAY_MODE_GAP)),
      OVERLAY_MODE_TILE_Y, OVERLAY_MODE_TILE_W, OVERLAY_MODE_TILE_H,
      m_palette->background, RADIUS_TRACK);
    lv_obj_set_style_border_width(overlayModeTile[i], 1, 0);
    lv_obj_set_style_border_color(overlayModeTile[i], m_palette->colourDisabled, 0);
  }
  // Same three words the status bar uses for the same three modes; FM_JOG is
  // deliberately absent (section 3: jog is no longer a mode).
  const char* modeNames[3] = { "FEED", "THREAD R", "THREAD L" };
  for (int i = 0; i < 3; i++) {
    overlayModeTileLabel[i] = createLabel(
      overlayModeGroup, &lv_font_montserrat_14, m_palette->textDim,
      OVERLAY_MODE_X0 + (i * (OVERLAY_MODE_TILE_W + OVERLAY_MODE_GAP)),
      OVERLAY_MODE_LABEL_Y);
    fixLabelBox(overlayModeTileLabel[i], OVERLAY_MODE_TILE_W,
                LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(overlayModeTileLabel[i], modeNames[i]);
  }

  // Group C: STOPS. Same construction as band 4 -- markers and carriage are
  // siblings of the track, not children, because they overhang it vertically
  // and a child would be clipped to the track's height.
  overlayStopsGroup = createOverlayGroup(overlayPanel);
  overlayStopsTrack = createRect(overlayStopsGroup, OVERLAY_STOP_TRACK_X,
                                 OVERLAY_STOP_TRACK_Y, OVERLAY_STOP_TRACK_W,
                                 OVERLAY_STOP_TRACK_H,
                                 m_palette->colourDisabled, RADIUS_TRACK);
  // End markers always drawn, ghosted when unset -- the same treatment (and
  // the same reason) as the band-4 marks above.
  overlayStopsLeftMark = createRect(overlayStopsGroup, OVERLAY_STOP_TRACK_X,
                                    OVERLAY_STOP_MARK_Y, OVERLAY_STOP_MARK_W,
                                    OVERLAY_STOP_MARK_H,
                                    m_palette->colourDisabled, 0);
  overlayStopsRightMark = createRect(
    overlayStopsGroup,
    OVERLAY_STOP_TRACK_X + OVERLAY_STOP_TRACK_W - OVERLAY_STOP_MARK_W,
    OVERLAY_STOP_MARK_Y, OVERLAY_STOP_MARK_W, OVERLAY_STOP_MARK_H,
    m_palette->colourDisabled, 0);
  overlayStopsCarriage = createRect(overlayStopsGroup, OVERLAY_STOP_TRACK_X,
                                    OVERLAY_STOP_MARK_Y,
                                    OVERLAY_STOP_CARRIAGE_W,
                                    OVERLAY_STOP_MARK_H,
                                    m_palette->textPrimary, RADIUS_TRACK);
  lv_obj_add_flag(overlayStopsCarriage, LV_OBJ_FLAG_HIDDEN);
  overlayStopsLeftLabel = createLabel(overlayStopsGroup,
                                      &lv_font_montserrat_14,
                                      m_palette->textDim, OVERLAY_STOP_LEFT_X,
                                      OVERLAY_STOP_LABEL_Y);
  fixLabelBox(overlayStopsLeftLabel, OVERLAY_STOP_END_W, LV_TEXT_ALIGN_LEFT);
  overlayStopsRightLabel = createLabel(overlayStopsGroup,
                                       &lv_font_montserrat_14,
                                       m_palette->textDim,
                                       OVERLAY_STOP_RIGHT_X,
                                       OVERLAY_STOP_LABEL_Y);
  fixLabelBox(overlayStopsRightLabel, OVERLAY_STOP_END_W, LV_TEXT_ALIGN_RIGHT);
  overlayStopsPosLabel = createLabel(overlayStopsGroup, &lv_font_montserrat_26,
                                     m_palette->textPrimary,
                                     OVERLAY_STOP_POS_X, OVERLAY_STOP_POS_Y);
  fixLabelBox(overlayStopsPosLabel, OVERLAY_STOP_POS_W, LV_TEXT_ALIGN_CENTER);
  // The clear-both confirm bar, built hidden (all three) to match the
  // m_lastStopsConfirmShown cache resetObjectTree() just cleared -- at rest the
  // STOPS body is exactly what it was before this bar existed. Creation order
  // is z-order: track, then the fill on top of it, then the words on top of
  // both. The fill is created at its floor width; drawOverlayStops() sizes it.
  // Track and fill share the travel track's x/width and RADIUS_TRACK so the
  // bar reads as the same span being consumed, and the fill is colourFault --
  // the destroy colour, not a neutral progress tint -- because completing this
  // hold erases both stop positions.
  overlayStopsConfirmTrack = createRect(overlayStopsGroup,
                                        OVERLAY_STOP_TRACK_X,
                                        OVERLAY_STOP_CONFIRM_Y,
                                        OVERLAY_STOP_TRACK_W,
                                        OVERLAY_STOP_CONFIRM_H,
                                        m_palette->colourDisabled,
                                        RADIUS_TRACK);
  lv_obj_add_flag(overlayStopsConfirmTrack, LV_OBJ_FLAG_HIDDEN);
  overlayStopsConfirmFill = createRect(overlayStopsGroup,
                                       OVERLAY_STOP_TRACK_X,
                                       OVERLAY_STOP_CONFIRM_Y,
                                       OVERLAY_STOP_CONFIRM_FILL_MIN,
                                       OVERLAY_STOP_CONFIRM_H,
                                       m_palette->colourFault, RADIUS_TRACK);
  lv_obj_add_flag(overlayStopsConfirmFill, LV_OBJ_FLAG_HIDDEN);
  // Static text, set once -- no TextSlot: "CLEARING BOTH STOPS" is 19 bytes,
  // flush against setLabelText()'s 19-byte cache cap, and a static string has
  // no business in the cache anyway. chipInk, the same punched-out ink as
  // every filled chip, keeps it >= 3.9:1 on the grey track and better on the
  // red fill in both palettes.
  overlayStopsConfirmLabel = createLabel(overlayStopsGroup,
                                         &lv_font_montserrat_14,
                                         m_palette->chipInk,
                                         OVERLAY_STOP_TRACK_X,
                                         OVERLAY_STOP_CONFIRM_LABEL_Y);
  fixLabelBox(overlayStopsConfirmLabel, OVERLAY_STOP_TRACK_W,
              LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(overlayStopsConfirmLabel, "CLEARING BOTH STOPS");
  lv_obj_add_flag(overlayStopsConfirmLabel, LV_OBJ_FLAG_HIDDEN);

  // Group D: MENU, the tile carousel -- three equal tiles across (prev /
  // current / next), the same geometry and quiet-tile treatment as the MODE
  // group so the two widgets read as one family. Unlike the MODE labels, the
  // names are CHILDREN of their tiles: they wrap to up to two lines, and being
  // a child both clips a wayward name to its tile and lets LV_ALIGN_CENTER
  // keep it vertically centred as the wrapped line count changes.
  // drawOverlayMenu() styles the three tiles from their MenuTileBlock states
  // (centre live = accent fill; any blocked tile = colourDisabled fill).
  // The position label lives in this group rather than beside overlayTitle so
  // it disappears with the rest of the menu instead of hanging over the
  // PITCH/MODE/STOPS widgets.
  overlayMenuGroup = createOverlayGroup(overlayPanel);
  overlayMenuPos = createLabel(overlayMenuGroup, &lv_font_montserrat_14,
                               m_palette->textDim, OVERLAY_MENU_POS_X,
                               OVERLAY_TITLE_Y);
  fixLabelBox(overlayMenuPos, OVERLAY_MENU_POS_W, LV_TEXT_ALIGN_RIGHT);
  for (int i = 0; i < 3; i++) {
    overlayMenuTile[i] = createRect(
      overlayMenuGroup,
      OVERLAY_MENU_X0 + (i * (OVERLAY_MENU_TILE_W + OVERLAY_MENU_GAP)),
      OVERLAY_MENU_TILE_Y, OVERLAY_MENU_TILE_W, OVERLAY_MENU_TILE_H,
      m_palette->background, RADIUS_TRACK);
    lv_obj_set_style_border_width(overlayMenuTile[i], 1, 0);
    lv_obj_set_style_border_color(overlayMenuTile[i], m_palette->colourDisabled, 0);
    overlayMenuTileLabel[i] = createLabel(overlayMenuTile[i],
                                          &lv_font_montserrat_14,
                                          m_palette->textDim, 0, 0);
    // Fixed width (so names wrap inside the tile), centred text, and WRAP --
    // deliberately NOT fixLabelBox(), whose CLIP long-mode would kill the
    // wrapping this layout depends on.
    lv_obj_set_width(overlayMenuTileLabel[i],
                     OVERLAY_MENU_TILE_W - (2 * OVERLAY_MENU_NAME_PAD));
    lv_obj_set_style_text_align(overlayMenuTileLabel[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(overlayMenuTileLabel[i], LV_ALIGN_CENTER, 0, 0);
  }

  // Group E: DRO DATUM -- two tiles on the MODE grammar, centred. Everything
  // inside a tile (name, mini bar, zero-post) is a CHILD of it, so the
  // selection restyle in drawOverlayDatum() recolours the whole tile as one
  // unit and the tile clips any wayward child. Both tiles are built in the
  // quiet-well style; the first drawOverlayDatum() paints the selection
  // (m_lastDatumTile is -1 after resetObjectTree()).
  overlayDatumGroup = createOverlayGroup(overlayPanel);
  for (int i = 0; i < 2; i++) {
    const int tileX =
      OVERLAY_DATUM_X0 + (i * (OVERLAY_DATUM_TILE_W + OVERLAY_DATUM_GAP));
    overlayDatumTile[i] = createRect(overlayDatumGroup, tileX,
                                     OVERLAY_DATUM_TILE_Y,
                                     OVERLAY_DATUM_TILE_W,
                                     OVERLAY_DATUM_TILE_H,
                                     m_palette->background, RADIUS_TRACK);
    lv_obj_set_style_border_width(overlayDatumTile[i], 1, 0);
    lv_obj_set_style_border_color(overlayDatumTile[i],
                                  m_palette->colourDisabled, 0);
    overlayDatumTileLabel[i] = createLabel(overlayDatumTile[i],
                                           &lv_font_montserrat_14,
                                           m_palette->textDim, 0,
                                           OVERLAY_DATUM_LABEL_Y);
    fixLabelBox(overlayDatumTileLabel[i], OVERLAY_DATUM_TILE_W,
                LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(overlayDatumTileLabel[i], i == 0 ? "LEFT" : "RIGHT");
    // The miniature travel bar, with the zero-post at THIS tile's end of
    // travel: left end on the LEFT tile, right end on the RIGHT tile.
    overlayDatumBar[i] = createRect(overlayDatumTile[i], OVERLAY_DATUM_BAR_X,
                                    OVERLAY_DATUM_BAR_Y, OVERLAY_DATUM_BAR_W,
                                    OVERLAY_DATUM_BAR_H,
                                    m_palette->colourDisabled, 0);
    overlayDatumZero[i] = createRect(
      overlayDatumTile[i],
      i == 0 ? OVERLAY_DATUM_POST_X_LEFT : OVERLAY_DATUM_POST_X_RIGHT,
      OVERLAY_DATUM_POST_Y, OVERLAY_DATUM_POST_W, OVERLAY_DATUM_POST_H,
      m_palette->textPrimary, 0);
  }

  // --- The Diagnostics screen (UiFocus::Diagnostics) ------------------------
  // A full-screen opaque panel, built like everything else -- once, hidden --
  // and only shown/hidden plus value-pushed at runtime. Created AFTER the
  // overlay panel so it sits above it in the sibling z-order (the read-only
  // screens replace the whole dashboard, overlays included). Static labels
  // (titles, units, the exit hint) are locals: set once, never touched again.
  diagPanel = createRect(lv_screen_active(), 0, 0, SCREEN_W, SCREEN_H,
                         m_palette->background, 0);
  lv_obj_add_flag(diagPanel, LV_OBJ_FLAG_HIDDEN);
  {
    // The title is the ONE label on this screen that is not static furniture:
    // it doubles as the capture-status line (see diagTitle in the header), so
    // it is a member with a fixed box rather than a local set once here.
    diagTitle = createLabel(diagPanel, &lv_font_montserrat_14,
                            m_palette->textDim, DIAG_TITLE_X, DIAG_TITLE_Y);
    fixLabelBox(diagTitle, DIAG_TITLE_W, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(diagTitle, "DIAGNOSTICS");
    lv_obj_t* el = createLabel(diagPanel, &lv_font_montserrat_14,
                               m_palette->textDim, DIAG_TITLE_X,
                               DIAG_ERR_LABEL_Y);
    lv_label_set_text(el, "POSITION ERROR");
    lv_obj_t* eu = createLabel(diagPanel, &lv_font_montserrat_26,
                               m_palette->textDim, DIAG_ERR_UNIT_X,
                               DIAG_ERR_UNIT_Y);
    lv_label_set_text(eu, "mm");
    const int colX[3] = { DIAG_COL_X0, DIAG_COL_X1, DIAG_COL_X2 };
    const char* colLabel[3] = { "SPINDLE", "CARRIAGE", "EXPECT" };
    const char* colUnit[3] = { "RPM", "mm/s", "mm/s" };
    for (int i = 0; i < 3; i++) {
      lv_obj_t* l = createLabel(diagPanel, &lv_font_montserrat_14,
                                m_palette->textDim, colX[i],
                                DIAG_RATE_LABEL_Y);
      lv_label_set_text(l, colLabel[i]);
      lv_obj_t* u = createLabel(diagPanel, &lv_font_montserrat_14,
                                m_palette->textDim, colX[i], DIAG_RATE_UNIT_Y);
      lv_label_set_text(u, colUnit[i]);
    }
    lv_obj_t* sl = createLabel(diagPanel, &lv_font_montserrat_14,
                               m_palette->textDim, DIAG_SYNC_LABEL_X,
                               DIAG_BOTTOM_Y);
    lv_label_set_text(sl, "SYNC");
    // The anchor SOURCE's static label; the value label is a member below.
    lv_obj_t* al = createLabel(diagPanel, &lv_font_montserrat_14,
                               m_palette->textDim, DIAG_ANCHOR_LABEL_X,
                               DIAG_BOTTOM_Y);
    lv_label_set_text(al, "ANCHOR");
    // Exit hint: the three keys that leave the screen, nothing more. The
    // arrows are inert here (uistate.cpp) and deliberately unmentioned. On
    // the TITLE row, not the bottom row -- the anchor readout took its slot
    // (see the DIAG_ANCHOR_* constants).
    lv_obj_t* h = createLabel(diagPanel, &lv_font_montserrat_14,
                              m_palette->textDim, DIAG_HINT_X, DIAG_TITLE_Y);
    fixLabelBox(h, DIAG_HINT_W, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "OK / MENU / HALT");
    createRect(diagPanel, 0, DIAG_RULE_Y, SCREEN_W, 1,
               m_palette->colourDisabled, 0);
    // The centre-zero bar: track, the fixed zero tick, then the marker on top.
    createRect(diagPanel, DIAG_BAR_X, DIAG_BAR_TRACK_Y, DIAG_BAR_W,
               DIAG_BAR_TRACK_H, m_palette->colourDisabled, RADIUS_TRACK);
    createRect(diagPanel, DIAG_TICK_X, DIAG_TICK_Y, DIAG_TICK_W, DIAG_TICK_H,
               m_palette->textDim, 0);
  }
  diagErrMarker = createRect(diagPanel, DIAG_TICK_X - ((DIAG_BAR_MARK_W - DIAG_TICK_W) / 2),
                             DIAG_BAR_MARK_Y, DIAG_BAR_MARK_W, DIAG_BAR_MARK_H,
                             m_palette->textPrimary, RADIUS_TRACK);
  diagErrValue = createLabel(diagPanel, &lv_font_montserrat_48,
                             m_palette->textPrimary, DIAG_ERR_VALUE_X,
                             DIAG_ERR_VALUE_Y);
  fixLabelBox(diagErrValue, DIAG_ERR_VALUE_W, LV_TEXT_ALIGN_RIGHT);
  // On the POSITION ERROR label's row, not the title row: the raw pulse count
  // is that value's alternate unit and should read as part of the same block.
  diagErrPulses = createLabel(diagPanel, &lv_font_montserrat_14,
                              m_palette->textDim, DIAG_ERR_PULSES_X,
                              DIAG_ERR_LABEL_Y);
  fixLabelBox(diagErrPulses, DIAG_ERR_PULSES_W, LV_TEXT_ALIGN_RIGHT);
  {
    const int colX[3] = { DIAG_COL_X0, DIAG_COL_X1, DIAG_COL_X2 };
    lv_obj_t** vals[3] = { &diagSpindleValue, &diagCarriageValue,
                           &diagExpectValue };
    for (int i = 0; i < 3; i++) {
      *vals[i] = createLabel(diagPanel, &lv_font_montserrat_26,
                             m_palette->textPrimary, colX[i],
                             DIAG_RATE_VALUE_Y);
      fixLabelBox(*vals[i], DIAG_COL_W, LV_TEXT_ALIGN_LEFT);
    }
  }
  // The sync chip, on the status bar's chip grammar: colourRun fill + chipInk
  // when SYNCED, no fill + textDim otherwise. Fill colour set once here; the
  // draw toggles opacity and ink only, like the status bar's SYNC chip.
  diagSyncChip = createLabel(diagPanel, &lv_font_montserrat_14,
                             m_palette->textDim,
                             DIAG_SYNC_CHIP_X - DIAG_SYNC_PAD_H,
                             DIAG_BOTTOM_Y - DIAG_SYNC_PAD_V);
  lv_obj_set_style_pad_hor(diagSyncChip, DIAG_SYNC_PAD_H, 0);
  lv_obj_set_style_pad_ver(diagSyncChip, DIAG_SYNC_PAD_V, 0);
  lv_obj_set_style_radius(diagSyncChip, RADIUS_TRACK, 0);
  lv_obj_set_style_bg_color(diagSyncChip, m_palette->colourRun, 0);
  lv_obj_set_style_bg_opa(diagSyncChip, LV_OPA_TRANSP, 0);
  // The anchor source value ("L stop" / "R stop" / "manual" / "none"), from
  // Leadscrew::getSyncAnchorState(). Plain primary ink, no chip: it is a
  // readout of WHERE the helix anchor came from, not a judgement -- the chip
  // to its left already carries the good/bad colour.
  diagAnchorValue = createLabel(diagPanel, &lv_font_montserrat_14,
                                m_palette->textPrimary, DIAG_ANCHOR_VALUE_X,
                                DIAG_BOTTOM_Y);
  fixLabelBox(diagAnchorValue, DIAG_ANCHOR_VALUE_W, LV_TEXT_ALIGN_RIGHT);

  // --- The About screen (UiFocus::About) ------------------------------------
  // Quiet and plain: three labelled values, the IP at 36 as the hero, and an
  // exit hint. Version is compile-time text, set once here and never cached.
  aboutPanel = createRect(lv_screen_active(), 0, 0, SCREEN_W, SCREEN_H,
                          m_palette->background, 0);
  lv_obj_add_flag(aboutPanel, LV_OBJ_FLAG_HIDDEN);
  {
    lv_obj_t* t = createLabel(aboutPanel, &lv_font_montserrat_14,
                              m_palette->textDim, ABOUT_TITLE_X, ABOUT_TITLE_Y);
    lv_label_set_text(t, "ABOUT");
    lv_obj_t* il = createLabel(aboutPanel, &lv_font_montserrat_14,
                               m_palette->textDim, ABOUT_IP_X,
                               ABOUT_IP_LABEL_Y);
    lv_label_set_text(il, "IP ADDRESS");
    lv_obj_t* fl = createLabel(aboutPanel, &lv_font_montserrat_14,
                               m_palette->textDim, ABOUT_FW_X,
                               ABOUT_ROW2_LABEL_Y);
    lv_label_set_text(fl, "FIRMWARE");
    lv_obj_t* fv = createLabel(aboutPanel, &lv_font_montserrat_26,
                               m_palette->textPrimary, ABOUT_FW_X,
                               ABOUT_ROW2_VALUE_Y);
    fixLabelBox(fv, ABOUT_FW_W, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(fv, FIRMWARE_VERSION);
    lv_obj_t* ul = createLabel(aboutPanel, &lv_font_montserrat_14,
                               m_palette->textDim, ABOUT_UP_X,
                               ABOUT_ROW2_LABEL_Y);
    lv_label_set_text(ul, "UPTIME");
    lv_obj_t* h = createLabel(aboutPanel, &lv_font_montserrat_14,
                              m_palette->textDim, ABOUT_HINT_X, ABOUT_HINT_Y);
    fixLabelBox(h, ABOUT_HINT_W, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "OK / MENU close");
  }
  // Auto-width on purpose, unlike every other variable readout: an IP must
  // never CLIP (a truncated address is worse than none), and the layout
  // asserts bound the widest possible address against the screen edge instead.
  aboutIpValue = createLabel(aboutPanel, &lv_font_montserrat_36,
                             m_palette->textPrimary, ABOUT_IP_X, ABOUT_IP_Y);
  aboutUptimeValue = createLabel(aboutPanel, &lv_font_montserrat_26,
                                 m_palette->textPrimary, ABOUT_UP_X,
                                 ABOUT_ROW2_VALUE_Y);
  fixLabelBox(aboutUptimeValue, ABOUT_UP_W, LV_TEXT_ALIGN_LEFT);
}

void showWifi(const char* ssid, const char* password, IPAddress ip) {

}


void Display::update() {
  //  tft.fillScreen(TFT_BLACK); // Rely on localised blanking to avoid blink, for now.
  if (GlobalState::getInstance()->getDisplayReset()) {
    init();
  }
  lv_timer_handler();

  // The two screens are mutually destructive: each one's builder now calls
  // lv_obj_clean() and drops the other's object pointers (they would otherwise
  // be left dangling at objects LVGL has deleted). So each branch checks that
  // the screen it is about to draw into actually exists, and rebuilds if not --
  // otherwise a getDisplayReset() during an update, or an OTA that ends without
  // the reboot, would push text into a null pointer.
  if (GlobalState::getInstance()->hasOTA()) {
    if (!initOta || updateLabel == nullptr) {
      initialiseOta();
    }
    drawOTA();

  } else {
    if (initOta || modeLabel == nullptr) {
      initOta = false;
      init();
    }
    drawStatusBar();
    drawSpindleRpm();
    drawMode();
    drawPitch();
    drawTravel();
    drawStateBar();
    // MUST stay after drawTravel(): the STOPS overlay renders the travel
    // figures out of the TS_TRAVEL_* caches that drawTravel() has just
    // refreshed, which is what guarantees the two bars agree (see
    // drawOverlayStops()). It is also drawn last because it sits on top.
    drawOverlay();
  }
  writeLed();
}

void Display::drawOTA() {
  GlobalState* state = GlobalState::getInstance();
  switch (state->getOtaStatus()) {
  case OTA_CHECKING:
  case OTA_IDLE:
    // OTA_IDLE is the brief window before the OTA task sets CHECKING.
    // "Checking updates...", not "Checking for updates...": at Montserrat 26
    // the longer form measured 306 of the 320 px -- flush to both edges,
    // unlike anything else on the panel. See TEXT26_OTA_STATUS_W.
    lv_label_set_text(updateLabel, "Checking updates...");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_NO_UPDATE:
    lv_label_set_text(updateLabel, "No update available");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_FAILED:
    lv_label_set_text(updateLabel, "Update failed");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_DOWNLOADING: {
    int bytes = state->getOTABytes();
    int length = state->getOTALength();
    int percent = length > 0 ? (int)(((float)(bytes * 100)) / ((float)length)) : 0;
    lv_slider_set_value(updateSlider, bytes > 0 ? percent : 0, LV_ANIM_OFF);
    if (percent > 99 && bytes > 0) {
      lv_label_set_text(updateLabel, "Rebooting...");
    } else {
      lv_label_set_text(updateLabel, "Updating...");
    }
    break;
  }
  }
}

// Band 1. Feed mode as text, the unit mode, and the sync indicator. The mode
// text carries the thread HAND ("THREAD R" / "THREAD L"), which is why the
// separate "L"/"R" label that used to sit over the mode glyph is gone.
// docs/ux-redesign.md section 8 uses a middle dot ("THREAD.R"); a plain space
// is used instead because LVGL's built-in Montserrat fonts carry ASCII plus the
// LV_SYMBOL range only -- U+00B7 would render as a placeholder box.
void Display::drawStatusBar() {
  const GlobalFeedMode mode = m_globalState->getFeedMode();
  const char* modeText;
  switch (mode) {
  case FM_THREAD:
    modeText = "THREAD R";
    break;
  case FM_THREAD_REVERSE:
    modeText = "THREAD L";
    break;
  case FM_JOG:
    modeText = "JOG";
    break;
  case FM_FEED:
  default:
    modeText = "FEED";
    break;
  }
  setLabelText(modeLabel, TS_MODE, modeText);

  const GlobalUnitMode unit = m_globalState->getUnitMode();
  setLabelText(unitLabel, TS_UNIT, unit == IMPERIAL ? "inch" : "mm");

  const GlobalThreadSyncState sync = m_globalState->getThreadSyncState();
  if ((int)sync != m_lastSyncState) {
    m_lastSyncState = (int)sync;
    // Synced = the chip lights (colourRun fill, set once in init(), chipInk
    // text); unsynced = no fill, textDim text. See the creation comment for
    // why this is a fill toggle and not a text-colour swap.
    const bool synced = (sync == SS_SYNC);
    lv_obj_set_style_bg_opa(syncLabel, synced ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(
      syncLabel, synced ? m_palette->chipInk : m_palette->textDim, 0);
  }
}

void Display::drawSpindleRpm() {
  int rrpm = (int)m_spindle->getEstimatedVelocityInRPM();
  int rpm = abs(rrpm);
  char rpmString[TEXT_SLOT_LEN];
  snprintf(rpmString, sizeof(rpmString), "%d", rpm);
  setLabelText(rpmLabel, TS_RPM, rpmString);
  // Reverse spindle is flagged by colouring the value. (This tested `rpm < 0`
  // once, which is abs() and therefore never true; and the colour was an
  // un-swapped 0xFF0000 literal, which renders BLUE on this R<->B-swapped
  // panel. Both are fixed: the signed value is tested and the colour comes from
  // the palette.)
  const bool negative = rrpm < 0;
  if (negative != m_lastRpmNegative) {
    m_lastRpmNegative = negative;
    lv_obj_set_style_text_color(
      rpmLabel, negative ? m_palette->colourFault : m_palette->textPrimary, 0);
  }
}

// Band 2, the 128x64 mode glyph.
void Display::drawMode() {
  const GlobalFeedMode mode = m_globalState->getFeedMode();
  const void* src;
  switch (mode) {
  case FM_THREAD:
    // Right-hand thread: crests lean "\" (a right-hand helix's front-facing
    // crests run top-left to bottom-right).
    src = &threadSymbol;
    break;
  case FM_THREAD_REVERSE:
    // Left-hand / reverse thread: the same glyph with the crests leaning "/"
    // (a separate generated asset -- this LVGL has no image-flip API).
    src = &threadSymbolReverse;
    break;
  // FM_JOG had its own glyph here. It stopped being reachable when jog left
  // the mode cycle (IncFeedMode(), section 3), and nothing else ever assigns
  // it, so it falls through to feedSymbol with the rest of default:.
  case FM_FEED:
  default:
    src = &feedSymbol;
    break;
  }
  if (src != m_lastFeedSrc) {
    m_lastFeedSrc = src;
    lv_image_set_src(feedSymbolObj, src);
  }
}

// Band 2 value + band 3 ticker.
//
// UNITS (docs/ux-redesign.md section 8): thread pitch reads mm metric / TPI
// imperial; feed pitch reads mm metric / THOU imperial; jog reads %. All of
// that -- which table, which unit word, which format -- now lives in
// currentPitchTable()/formatPitch() above, because the RATE and JOG SPEED
// overlays have to render the identical thing from the identical source. Read
// the warning there about getCurrentFeedPitch() before touching any of it.
void Display::drawPitch() {
  // Exactly what the RATE overlay renders, from the same call -- this readout
  // and that widget are the same number and must never disagree.
  int tickerIndex = 0;
  const PitchTable table = tickerTable(m_globalState, false, tickerIndex);

  char value[TEXT_SLOT_LEN];
  formatPitch(value, sizeof(value), table, tickerIndex);

  const bool valueChanged = setLabelText(pitchLabel, TS_PITCH, value);
  setLabelText(pitchUnitLabel, TS_PITCH_UNIT, table.unit);
  // The unit hangs off the right-hand edge of the value, so it only has to move
  // when the VALUE's width changes. Doing this unconditionally would re-position
  // (and so invalidate) it on every one of the 10 ticks a second.
  if (valueChanged) {
    lv_obj_align_to(pitchUnitLabel, pitchLabel, LV_ALIGN_OUT_RIGHT_BOTTOM,
                    PITCH_UNIT_GAP, PITCH_UNIT_BASELINE_FIX);
  }

  // --- band 3: the pip row --------------------------------------------------
  // Count clamped to what init() built; the static_asserts there make the
  // clamp unreachable for the real tables, so this only guards a corrupt
  // count. Two gates, both essential at 10 Hz:
  //   * count change -> REFLOW: spread `count` pips evenly across the row
  //     (positions depend on count) and hide the surplus. Rare -- only a
  //     mode/unit change alters the list length (20 for the four pitch
  //     tables, 6 for jogSpeeds).
  //   * index change -> RESTYLE: promote current + neighbours, demote the
  //     rest. The reflow forces it by resetting the index cache, because it
  //     has just re-homed (and re-shortened) every pip.
  const int pipCount = table.count > PIP_MAX ? PIP_MAX : table.count;
  if (pipCount != m_lastPipCount) {
    m_lastPipCount = pipCount;
    m_lastPipIndex = -2;  // -2, not -1: -1 must stay a valid "no selection"
                          // (index 0's neighbour set differs from none at all)
    const int span = PIP_ROW_W - PIP_W;
    for (int i = 0; i < PIP_MAX; i++) {
      if (i < pipCount) {
        const int x = pipCount > 1
          ? PIP_ROW_X + ((i * span) / (pipCount - 1))
          : PIP_ROW_X + (span / 2);
        lv_obj_set_pos(pitchPips[i], x, PIP_BASE_Y - PIP_H_OTHER);
        lv_obj_remove_flag(pitchPips[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(pitchPips[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  if (tickerIndex != m_lastPipIndex) {
    m_lastPipIndex = tickerIndex;
    for (int i = 0; i < pipCount; i++) {
      // Height + colour carry the whole encoding: tall accent = current,
      // mid textDim = its neighbours, short colourDisabled = the rest. Sizes
      // are set with the position so every pip keeps the common baseline.
      int h;
      lv_color_t colour;
      if (i == tickerIndex) {
        h = PIP_H_CURRENT;
        colour = m_palette->accent;
      } else if (i == tickerIndex - 1 || i == tickerIndex + 1) {
        h = PIP_H_NEIGHBOUR;
        colour = m_palette->textDim;
      } else {
        h = PIP_H_OTHER;
        colour = m_palette->colourDisabled;
      }
      lv_obj_set_size(pitchPips[i], PIP_W, h);
      lv_obj_set_y(pitchPips[i], PIP_BASE_Y - h);
      lv_obj_set_style_bg_color(pitchPips[i], colour, 0);
    }
  }
}

// Band 4 -- the carriage travel bar, a small DRO.
//
// docs/ux-redesign.md section 8, "The DRO datum": a position is meaningless
// without a zero, so zero is referenced to an endstop and the rules live in
// lib/dro (host-tested). Everything shown here is DATUM-RELATIVE, which is why
// the datum end always reads 0.00 -- it is emphasised (textPrimary) while the
// far end and the live value's unit stay dimmed.
//
// The INT32_MIN/INT32_MAX unset sentinels are translated into DroInput's
// booleans here, and an unset stop's stored pulses are never passed on: that
// translation is the caller's job by contract (lib/dro never sees a sentinel).
//
// Two things this deliberately does NOT do yet:
//   * Manual zero (OK-held) has no store anywhere in the firmware, so
//     manualZeroSet is hard-false. It belongs with the OK-hold gesture in the
//     button/focus rework (FS-I3 and the buttonpad rewrite), and it must be a
//     DISPLAY datum -- NOT Leadscrew::setCurrentPosition(), because the stops
//     are stored absolute against that same counter and rezeroing it would
//     silently shift every stop relative to the tool.
//   * "the readout flashes for ~1 s whenever the datum moves" -- animation is
//     out (10 FPS, section 8 "Renderer constraints"), so the datum change is
//     shown statically by which end is emphasised.
// The doc's `REL` tag for the origin datum is likewise not drawn: with manual
// zero absent, the origin is reached only when NEITHER stop is set, and that
// state is already unambiguous on screen -- both ends read "--".
void Display::drawTravel() {
  const LatheConfigDerived* cfg = m_leadscrew->getConfig();
  const bool imperial = (m_globalState->getUnitMode() == IMPERIAL);
  const float stepsPerMm = cfg->leadscrewStepsPerMm();

  const bool leftSet = m_leadscrew->getStopPositionState(
                         LeadscrewStopPosition::LEFT) == LeadscrewStopState::SET;
  const bool rightSet = m_leadscrew->getStopPositionState(
                          LeadscrewStopPosition::RIGHT) == LeadscrewStopState::SET;

  DroInput dro;
  dro.leftStopSet = leftSet;
  dro.leftStopPulses = leftSet
    ? m_leadscrew->getStopPosition(LeadscrewStopPosition::LEFT) : 0;
  dro.rightStopSet = rightSet;
  dro.rightStopPulses = rightSet
    ? m_leadscrew->getStopPosition(LeadscrewStopPosition::RIGHT) : 0;
  dro.manualZeroSet = false;  // see the note above -- no store for it yet.
  dro.manualZeroPulses = 0;
  // m_droDatum, not cfg->droDatum(): the "DRO datum" menu tile changes the
  // preference at runtime and cannot write through to LatheConfig (see the
  // member's comment in the header). It is seeded FROM cfg in the constructor,
  // so this reads identically until the tile is used.
  dro.preference = m_droDatum;

  const DroDatumSource source = Dro::resolveSource(dro);
  const float safeStepsPerMm = (stepsPerMm > 0.0f) ? stepsPerMm : 1.0f;
  const float datumMM = Dro::datumPulses(dro) / safeStepsPerMm;
  const float positionMM =
    Dro::relativePulses(dro, m_leadscrew->getCurrentPosition()) / safeStepsPerMm;

  char value[TEXT_SLOT_LEN];
  char text[TEXT_SLOT_LEN];

  formatTravelValue(value, sizeof(value), positionMM, imperial);
  setLabelText(travelPosLabel, TS_TRAVEL_POS, value);
  setLabelText(travelPosUnit, TS_TRAVEL_UNIT, imperial ? "in" : "mm");

  // Each end shows THAT STOP's own position (relative to the datum), or "--"
  // when it is unset. getStopPositionMM() returns absolute mm and is only valid
  // while the stop is SET, which the flags above have already established.
  if (leftSet) {
    formatTravelValue(value, sizeof(value),
                      m_leadscrew->getStopPositionMM(LeadscrewStopPosition::LEFT)
                        - datumMM,
                      imperial);
    snprintf(text, sizeof(text), "L %s", value);
  } else {
    snprintf(text, sizeof(text), "L --");
  }
  setLabelText(travelLeftLabel, TS_TRAVEL_LEFT, text);

  if (rightSet) {
    formatTravelValue(value, sizeof(value),
                      m_leadscrew->getStopPositionMM(LeadscrewStopPosition::RIGHT)
                        - datumMM,
                      imperial);
    snprintf(text, sizeof(text), "%s R", value);
  } else {
    snprintf(text, sizeof(text), "-- R");
  }
  setLabelText(travelRightLabel, TS_TRAVEL_RIGHT, text);

  // Datum emphasis. Coincident stops still resolve to the preferred SIDE, so
  // this follows the resolved source rather than the positions.
  if ((int)source != m_lastDatumSource) {
    m_lastDatumSource = (int)source;
    lv_obj_set_style_text_color(travelLeftLabel,
      source == DroDatumSource::LeftStop ? m_palette->textPrimary
                                         : m_palette->textDim, 0);
    lv_obj_set_style_text_color(travelRightLabel,
      source == DroDatumSource::RightStop ? m_palette->textPrimary
                                          : m_palette->textDim, 0);
  }

  // Stop markers: always drawn; SET is colourRun, unset is a colourDisabled
  // ghost tick (see the creation comment -- a hidden end made the band read
  // as an empty progress bar).
  if (leftSet != m_lastLeftStopSet) {
    m_lastLeftStopSet = leftSet;
    lv_obj_set_style_bg_color(travelLeftMark,
                              leftSet ? m_palette->colourRun
                                      : m_palette->colourDisabled, 0);
  }
  if (rightSet != m_lastRightStopSet) {
    m_lastRightStopSet = rightSet;
    lv_obj_set_style_bg_color(travelRightMark,
                              rightSet ? m_palette->colourRun
                                       : m_palette->colourDisabled, 0);
  }

  // The carriage marker needs a SPAN to sit in, and only two set stops provide
  // one; carriageFraction() owns that rule (and is shared with the STOPS
  // overlay). It is hidden when there is no span; the numeric readout still
  // shows where the carriage is.
  bool showCarriage = false;
  int carriageX = m_lastCarriageX;
  float fraction = 0.0f;
  if (carriageFraction(m_leadscrew, leftSet, rightSet, fraction)) {
    carriageX = TRAVEL_TRACK_X +
      (int)(fraction * (float)(TRAVEL_TRACK_W - TRAVEL_CARRIAGE_W));
    showCarriage = true;
  }
  if (showCarriage && carriageX != m_lastCarriageX) {
    m_lastCarriageX = carriageX;
    lv_obj_set_pos(travelCarriage, carriageX, TRAVEL_MARK_Y);
  }
  if (showCarriage != m_lastCarriageShown) {
    m_lastCarriageShown = showCarriage;
    if (showCarriage) {
      lv_obj_remove_flag(travelCarriage, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(travelCarriage, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// Band 5 -- the machine state chip: the word at 36 on a state-coloured fill.
// The text stays chipInk (set once in init()); only the word and the fill
// colour move. The soft-key hints that used to share this band are gone.
//
// MM_JOG_* is the powered run to a stop ("RETURNING"); MM_INTERACTIVE_JOG_* is
// the hold-to-move dead-man jog, which shows the direction it is travelling.
void Display::drawStateBar() {
  const GlobalMotionMode mode = m_globalState->getMotionMode();
  const char* word;
  lv_color_t colour;
  switch (mode) {
  case MM_ENABLED:
    word = "CUTTING";
    colour = m_palette->colourRun;
    break;
  case MM_INTERACTIVE_JOG_LEFT:
    word = "JOG " LV_SYMBOL_LEFT;
    colour = m_palette->colourCaution;
    break;
  case MM_INTERACTIVE_JOG_RIGHT:
    word = "JOG " LV_SYMBOL_RIGHT;
    colour = m_palette->colourCaution;
    break;
  case MM_JOG_LEFT:
  case MM_JOG_RIGHT:
    word = "RETURNING";
    colour = m_palette->colourCaution;
    break;
  case MM_DECELLERATE:
    word = "HALTED";
    colour = m_palette->colourFault;
    break;
  case MM_DISABLED:
  case MM_UNSET:
  default:
    word = "IDLE";
    colour = m_palette->colourDisabled;
    break;
  }

  setLabelText(stateLabel, TS_STATE, word);
  if ((int)mode != m_lastMotionMode) {
    m_lastMotionMode = (int)mode;
    lv_obj_set_style_bg_color(stateLabel, colour, 0);
  }

  updateLed();
}

// --- The selector overlay (docs/ux-redesign.md section 4) --------------------
//
// One panel for all five overlay focuses (the four selectors plus the menu),
// its CONTENTS swapped rather than the panel rebuilt. Show/hide is
// LV_OBJ_FLAG_HIDDEN only -- never a delete and
// re-create: this runs at 10 Hz, and rebuilding would both burn the frame and
// throw away every redraw cache below.
//
// UiFocus::Jog hides it: the main screen IS the jog view, and section 3 has the
// arrows driving the carriage there, so nothing should be covering the travel
// bar while that is true.
//
// UiFocus::Menu uses the SAME panel, with the carousel as its content group
// (docs/ux-redesign.md section 6). It is a settings surface like the other four,
// so it obeys the same rule: it covers bands 2-4 and never the status bar or the
// state bar, so RPM and the CUTTING/HALTED word stay readable throughout.
void Display::drawOverlay() {
  if (overlayPanel == nullptr) {
    return;
  }
  // No ButtonPad on the Wi-Fi setup path, so no focus: behave as if resting.
  const UiFocus focus = (m_ui != nullptr) ? m_ui->focus() : UiFocus::Jog;

  // Stop edits are refused outright while the carriage is under power. This is
  // NOT a display rule -- it mirrors UiState (uistate.cpp, UiFocus::Stops:
  // `if (ctx.motionEnabled || ctx.motionActive) return UiIntent::None`), and the
  // same expression ButtonPad::buildContext() builds motionActive from, so the
  // hint cannot end up advertising a gesture the machine will ignore.
  // motionEnabled (MM_ENABLED) is a subset of motionActive, so testing the
  // broader one alone is exactly equivalent.
  const GlobalMotionMode motion = m_globalState->getMotionMode();
  const bool stopsLocked = (motion != MM_DISABLED && motion != MM_UNSET);

  // Whether the SELECTED menu tile can act right now, for the hint row.
  // `stopsLocked` is exactly the motionActive predicate menuTileBlock() wants
  // (same expression, same two exclusions), so it is reused rather than
  // recomputed. drawOverlayMenu() calls the same function again for each of the
  // three visible tiles; that is not a second opinion, because menuTileBlock()
  // is a pure function of these two bools and one index and is defined once.
  const GlobalFeedMode feedMode = m_globalState->getFeedMode();
  const bool threadMode = (feedMode == FM_THREAD || feedMode == FM_THREAD_REVERSE);
  const MenuTileBlock menuBlock =
    (m_ui != nullptr)
      ? menuTileBlock(m_ui->menuIndex(), stopsLocked, threadMode)
      : MTB_NONE;

  lv_obj_t* group = nullptr;
  // The two read-only screens (Diagnostics / About) are NOT overlay groups:
  // they live on their own full-screen panels, above everything. Exactly one
  // of `group` / `screen` can be non-null per focus.
  lv_obj_t* screen = nullptr;
  const char* title = "";
  OverlayHint hint = OH_NONE;
  // The clear-both hold, 0..1000 (0 = not running). Sampled ONCE, here, and
  // handed to drawOverlayStops(), so the hint row and the bar agree about the
  // hold within a tick. UiState only arms it when the gesture can succeed (at
  // rest, at least one stop set), so a non-zero value IS "this will clear both
  // stops if it completes" -- no precondition is re-derived here. The one
  // display-side guard, stopsLocked, mirrors the Hold handler's own fresh
  // re-check for motion that starts DURING the second (uistate.cpp): UiState
  // will refuse that hold when it lands, so the bar must stop filling for it
  // -- the locked hint takes over instead.
  int stopsConfirm = 0;
  switch (focus) {
  case UiFocus::Rate:
    group = overlayTickerGroup;
    title = "PITCH";
    hint = OH_PITCH;
    break;
  case UiFocus::JogSpeed:
    group = overlayTickerGroup;
    title = "JOG SPEED";
    hint = OH_SPEED;
    break;
  case UiFocus::Mode:
    group = overlayModeGroup;
    title = "MODE";
    hint = OH_MODE;
    break;
  case UiFocus::Stops:
    group = overlayStopsGroup;
    title = "STOPS";
    if (!stopsLocked && m_ui != nullptr) {
      stopsConfirm = m_ui->stopsConfirmPermille(millis());
    }
    hint = stopsLocked ? OH_STOPS_LOCKED
           : (stopsConfirm > 0) ? OH_STOPS_CONFIRM
                                : OH_STOPS;
    break;
  case UiFocus::DroDatum:
    // Same locked treatment (and the SAME predicate) as STOPS: UiState refuses
    // the datum arrows while the carriage is under power, so the row must
    // report the refusal, not offer dead arrows. The tile that OPENS this
    // widget is refused under power too (menuTileBlock), but motion can start
    // underneath an already-open picker -- a run finishing its deceleration,
    // the web UI -- which is exactly when this hint earns its keep.
    group = overlayDatumGroup;
    title = "DRO DATUM";
    hint = stopsLocked ? OH_DATUM_LOCKED : OH_DATUM;
    break;
  case UiFocus::Menu:
    // UiState keeps menuOpen() and Menu focus in lockstep, so focus alone is
    // the condition - the same single source the other four branches use.
    group = overlayMenuGroup;
    title = "MENU";
    hint = (menuBlock == MTB_MOTION)      ? OH_MENU_MOVING
           : (menuBlock == MTB_FEED_MODE) ? OH_MENU_FEED
                                          : OH_MENU;
    break;
  case UiFocus::Diagnostics:
    screen = diagPanel;
    break;
  case UiFocus::About:
    screen = aboutPanel;
    break;
  case UiFocus::Jog:
  default:
    break;
  }

  // Group + title + panel visibility all change together, and only on a focus
  // change -- lv_label_set_text() and the HIDDEN flag both invalidate
  // unconditionally, so none of it may run per tick.
  if ((int)focus != m_lastFocus) {
    m_lastFocus = (int)focus;
    lv_obj_add_flag(overlayTickerGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlayModeGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlayStopsGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlayMenuGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlayDatumGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(diagPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(aboutPanel, LV_OBJ_FLAG_HIDDEN);
    if (group != nullptr) {
      lv_label_set_text(overlayTitle, title);
      lv_obj_remove_flag(group, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(overlayPanel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(overlayPanel, LV_OBJ_FLAG_HIDDEN);
      if (screen != nullptr) {
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  // The read-only screens carry their own titles and hints, so the overlay's
  // shared hint machinery below is not theirs; push their values and be done.
  if (screen == diagPanel && screen != nullptr) {
    drawDiagnostics();
    return;
  }
  if (screen == aboutPanel && screen != nullptr) {
    drawAbout();
    return;
  }

  if (group == nullptr) {
    return;  // hidden: nothing to push, and the caches stay as they were.
  }

  // The hint is a literal picked by variant, so the variant is what is cached
  // (see the TextSlot note in the header). It changes on focus AND, for STOPS,
  // on the machine starting or stopping underneath a panel that is already up.
  if ((int)hint != m_lastHintVariant) {
    m_lastHintVariant = (int)hint;
    lv_label_set_text(overlayHintLeft, overlayHintText(hint));
    // A refusal is a caution, not an instruction, in both widgets that can
    // report one: the stops inhibit and the two menu blocks. It renders as a
    // colourCaution CHIP (fill on, chipInk text) rather than caution-coloured
    // text: caution text on the light surface is 1.5:1, chipInk on the fill
    // is ~10:1 in both palettes. The fill colour is set once in init(); only
    // the opacity and ink toggle here.
    const bool blocked = (hint == OH_STOPS_LOCKED || hint == OH_MENU_MOVING ||
                          hint == OH_MENU_FEED || hint == OH_DATUM_LOCKED);
    lv_obj_set_style_bg_opa(overlayHintLeft,
                            blocked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(overlayHintLeft,
                                blocked ? m_palette->chipInk
                                        : m_palette->textDim, 0);
    // The right half moves with the same variant now that it is no longer
    // always "OK done" -- see overlayHintOkText().
    lv_label_set_text(overlayHintRight, overlayHintOkText(hint));
  }

  switch (focus) {
  case UiFocus::Rate:
    drawOverlayTicker(false);
    break;
  case UiFocus::JogSpeed:
    drawOverlayTicker(true);
    break;
  case UiFocus::Mode:
    drawOverlayMode();
    break;
  case UiFocus::DroDatum:
    drawOverlayDatum();
    break;
  case UiFocus::Menu:
    drawOverlayMenu(stopsLocked, threadMode);
    break;
  case UiFocus::Stops:
  default:
    drawOverlayStops(stopsConfirm);
    break;
  }
}

// DRO DATUM. Two tiles, the PERSISTED end filled with the focus accent --
// m_droDatum is the single source of truth (UiState holds no pending copy; the
// arrows apply live through ButtonPad -> setDroDatum(), which is also what
// makes rendering the persisted value automatically render the selection).
// Same restyle grammar as the MODE tiles, extended to the mini bar and
// zero-post each tile carries: they are children of the tile, and on the
// accent fill they swap to chipInk for the same contrast reason every other
// chip does (a textPrimary post on the accent is invisible in the light
// palette).
void Display::drawOverlayDatum() {
  const int tile = (m_droDatum == DroDatumPreference::Right) ? 1 : 0;
  if (tile == m_lastDatumTile) {
    return;
  }
  m_lastDatumTile = tile;
  for (int i = 0; i < 2; i++) {
    const bool selected = (i == tile);
    lv_obj_set_style_bg_color(overlayDatumTile[i],
                              selected ? m_palette->accent
                                       : m_palette->background, 0);
    lv_obj_set_style_border_color(overlayDatumTile[i],
                                  selected ? m_palette->accent
                                           : m_palette->colourDisabled, 0);
    lv_obj_set_style_text_color(overlayDatumTileLabel[i],
                                selected ? m_palette->chipInk
                                         : m_palette->textDim, 0);
    lv_obj_set_style_bg_color(overlayDatumBar[i],
                              selected ? m_palette->chipInk
                                       : m_palette->colourDisabled, 0);
    lv_obj_set_style_bg_color(overlayDatumZero[i],
                              selected ? m_palette->chipInk
                                       : m_palette->textPrimary, 0);
  }
}

// Diagnostics (UiFocus::Diagnostics). Values only -- the panel and its static
// furniture were built in init(). Everything pushed here is gated on a cache:
// the strings through setLabelText()'s slots, the marker through its x/pegged
// pair, the sync chip through its state int.
void Display::drawDiagnostics() {
  // The title row doubles as the motion-trace capture's status line. Reads
  // "DIAGNOSTICS" whenever no capture exists, so the screen is unchanged for
  // anyone not using the instrument; while one does exist it is the only place
  // that says whether it is recording, waiting for the carriage to stop, or
  // has already sent. All three states are read straight off GlobalState (the
  // SpindleTask fills the trace, the upload task sends it, this runs on the
  // DisplayTask - no locks, see CLAUDE.md), and the wording is decided in
  // lib/global_state so its length can be held to this row's cache slot by the
  // host tests.
  {
    DebugCapture& dbg = m_globalState->debug();
    char title[TEXT_SLOT_LEN];
    formatCaptureStatus(title, sizeof(title), (int)dbg.state(), dbg.count(),
                        dbg.capacity());
    setLabelText(diagTitle, TS_DIAG_TITLE, title);
  }

  const float stepsPerMm = m_leadscrew->getConfig()->leadscrewStepsPerMm();
  const float safeStepsPerMm = (stepsPerMm > 0.0f) ? stepsPerMm : 1.0f;
  const float errPulses = m_leadscrew->getPositionError();
  float errMM = errPulses / safeStepsPerMm;

  char buf[TEXT_SLOT_LEN];
  // Clamp BEFORE formatting: past +-999.99 mm the magnitude is meaningless
  // (something is catastrophically wrong) and an unclamped value would
  // overflow the 48's measured box. The bar pegs (and turns colourFault) long
  // before this clamp is reachable, so nothing is hidden by it.
  if (errMM > 999.99f) {
    errMM = 999.99f;
  } else if (errMM < -999.99f) {
    errMM = -999.99f;
  }
  snprintf(buf, sizeof(buf), "%+.2f", (double)errMM);
  setLabelText(diagErrValue, TS_DIAG_ERR, buf);

  int p = (int)errPulses;
  if (p > 99999) {
    p = 99999;
  } else if (p < -99999) {
    p = -99999;
  }
  snprintf(buf, sizeof(buf), "%+d p", p);
  setLabelText(diagErrPulses, TS_DIAG_ERR_P, buf);

  // The centre-zero bar: deflection is error / full-scale, clamped to the
  // ends. Pegged = off scale, and the marker says so in colourFault -- the one
  // judgement this screen makes, and it is "beyond this instrument's range",
  // not "your thread is ruined".
  float frac = errMM / DIAG_BAR_FULL_SCALE_MM;
  bool pegged = false;
  if (frac > 1.0f) {
    frac = 1.0f;
    pegged = true;
  } else if (frac < -1.0f) {
    frac = -1.0f;
    pegged = true;
  }
  const int half = (DIAG_BAR_W - DIAG_BAR_MARK_W) / 2;
  const int markerX = DIAG_BAR_X + half + (int)(frac * (float)half);
  if (markerX != m_lastDiagErrX) {
    m_lastDiagErrX = markerX;
    lv_obj_set_pos(diagErrMarker, markerX, DIAG_BAR_MARK_Y);
  }
  if (pegged != m_lastDiagErrPegged) {
    m_lastDiagErrPegged = pegged;
    lv_obj_set_style_bg_color(diagErrMarker,
                              pegged ? m_palette->colourFault
                                     : m_palette->textPrimary, 0);
  }

  // The three rates. SPINDLE is the signed RPM (same source and cast as the
  // status bar). CARRIAGE is the measured leadscrew rate with its sign taken
  // from the commanded direction. EXPECT is what the carriage SHOULD do at
  // this spindle speed and pitch -- |RPM|/60 x |mm/rev| -- and reads "--"
  // unless the axis is engaged, because at rest a live expectation would
  // present the (legitimately) stationary carriage as a fault.
  const int rrpm = (int)m_spindle->getEstimatedVelocityInRPM();
  snprintf(buf, sizeof(buf), "%d", rrpm);
  setLabelText(diagSpindleValue, TS_DIAG_RPM, buf);

  const float rate = m_leadscrew->getEstimatedVelocityInMillimetersPerSecond();
  const float signedRate =
    (m_leadscrew->getCurrentDirection() == LeadscrewDirection::LEFT) ? -rate
                                                                     : rate;
  snprintf(buf, sizeof(buf), "%.2f", (double)signedRate);
  setLabelText(diagCarriageValue, TS_DIAG_CARRIAGE, buf);

  if (m_globalState->getMotionMode() == MM_ENABLED) {
    // getCurrentFeedPitch() is mm/rev in EVERY mode (the display caution in
    // currentPitchTable() is about UNIT RENDERING, not about this): exactly
    // what a rate expectation needs. Magnitude is |RPM|/60 x |mm/rev|; the
    // SIGN is copied from the same commanded direction CARRIAGE uses, so that
    // on a healthy cut the two columns read as the same number -- an
    // unsigned expectation next to a signed measurement would present every
    // leftward pass as a permanent "mismatch".
    float expect = (fabsf((float)rrpm) / 60.0f) *
                   fabsf(m_globalState->getCurrentFeedPitch());
    if (m_leadscrew->getCurrentDirection() == LeadscrewDirection::LEFT) {
      expect = -expect;
    }
    snprintf(buf, sizeof(buf), "%.2f", (double)expect);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  setLabelText(diagExpectValue, TS_DIAG_EXPECT, buf);

  // The helix sync state, on the status bar's chip grammar
  // (GlobalThreadSyncState: synced or not) ...
  const GlobalThreadSyncState sync = m_globalState->getThreadSyncState();
  if ((int)sync != m_lastDiagSyncState) {
    m_lastDiagSyncState = (int)sync;
    const bool synced = (sync == SS_SYNC);
    lv_obj_set_style_bg_opa(diagSyncChip,
                            synced ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(diagSyncChip,
                                synced ? m_palette->chipInk
                                       : m_palette->textDim, 0);
  }
  setLabelText(diagSyncChip, TS_DIAG_SYNC,
               sync == SS_SYNC ? "SYNCED" : "NOT SYNCED");

  // ... and, beside it, WHERE the anchor came from -- the bit the chip does
  // not carry. A thread that will pick up on a recut and one that will not
  // look identical without this: both read SYNCED, but only the anchor source
  // says what the helix is actually pinned to. Single aligned enum read from
  // the DisplayTask (see getSyncAnchorState()'s doc); the strings are all
  // static literals well inside the TEXT_SLOT_LEN cache cap.
  const char* anchorText;
  switch (m_leadscrew->getSyncAnchorState()) {
  case LeadscrewSpindleSyncPositionState::LEFT:
    anchorText = "L stop";
    break;
  case LeadscrewSpindleSyncPositionState::RIGHT:
    anchorText = "R stop";
    break;
  case LeadscrewSpindleSyncPositionState::MANUAL:
    anchorText = "manual";
    break;
  case LeadscrewSpindleSyncPositionState::UNSET:
  default:
    anchorText = "none";
    break;
  }
  setLabelText(diagAnchorValue, TS_DIAG_ANCHOR, anchorText);
}

// About (UiFocus::About). The IP is the hero; version was set in init() and
// never changes; uptime ticks once a second at most (the cache absorbs the
// other nine redraws).
void Display::drawAbout() {
  bool connected;
  char ip[TEXT_SLOT_LEN];
#if PIO_UNIT_TESTING
  connected = m_aboutConnected;
  snprintf(ip, sizeof(ip), "%s", m_aboutIp.toString().c_str());
#else
  connected = (WiFi.status() == WL_CONNECTED);
  snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
#endif
  if (!connected) {
    snprintf(ip, sizeof(ip), "not connected");
  }
  setLabelText(aboutIpValue, TS_ABOUT_IP, ip);
  const int connInt = connected ? 1 : 0;
  if (connInt != m_lastAboutConnected) {
    m_lastAboutConnected = connInt;
    lv_obj_set_style_text_color(aboutIpValue,
                                connected ? m_palette->textPrimary
                                          : m_palette->textDim, 0);
  }

  char up[TEXT_SLOT_LEN];
  const unsigned long secs = millis() / 1000UL;
  if (secs >= 86400UL) {
    snprintf(up, sizeof(up), "%lud %luh", secs / 86400UL,
             (secs % 86400UL) / 3600UL);
  } else if (secs >= 3600UL) {
    snprintf(up, sizeof(up), "%luh %lum", secs / 3600UL,
             (secs % 3600UL) / 60UL);
  } else {
    snprintf(up, sizeof(up), "%lum %lus", secs / 60UL, secs % 60UL);
  }
  setLabelText(aboutUptimeValue, TS_ABOUT_UPTIME, up);
}

// RATE and JOG SPEED. Same widget, different list: the current entry large in
// the centre with its unit under it, and the entry either side of it dimmed at
// 26 so the direction each arrow will move you in is visible before you press.
// At the ends of the list the outer label is simply blank (formatPitch() writes
// an empty string for an out-of-range index) rather than wrapping, because the
// underlying next/prevFeedPitch() saturate rather than wrapping too.
void Display::drawOverlayTicker(bool jogSpeed) {
  int index = 0;
  const PitchTable table = tickerTable(m_globalState, jogSpeed, index);

  char buf[TEXT_SLOT_LEN];
  formatPitch(buf, sizeof(buf), table, index - 1);
  setLabelText(overlayTickerPrev, TS_OV_TICK_PREV, buf);
  formatPitch(buf, sizeof(buf), table, index);
  setLabelText(overlayTickerValue, TS_OV_TICK_VALUE, buf);
  formatPitch(buf, sizeof(buf), table, index + 1);
  setLabelText(overlayTickerNext, TS_OV_TICK_NEXT, buf);
  setLabelText(overlayTickerUnit, TS_OV_TICK_UNIT, table.unit);
}

// MODE. Three tiles, the current one filled with the focus accent.
void Display::drawOverlayMode() {
  // Tile order matches the IncFeedMode() cycle (FEED -> THREAD ->
  // THREAD_REVERSE -> FEED), so the arrows move the fill along the row.
  const GlobalFeedMode mode = m_globalState->getFeedMode();
  int tile;
  switch (mode) {
  case FM_THREAD:
    tile = 1;
    break;
  case FM_THREAD_REVERSE:
    tile = 2;
    break;
  case FM_FEED:
    tile = 0;
    break;
  default:
    // FM_JOG has no tile: jog stopped being a mode in section 3. Nothing is
    // filled rather than something being filled wrongly.
    tile = -1;
    break;
  }

  if (tile == m_lastModeTile) {
    return;
  }
  m_lastModeTile = tile;
  for (int i = 0; i < 3; i++) {
    const bool selected = (i == tile);
    // Selected: accent fill + chipInk (a dark ink in BOTH palettes -- the
    // accent is mid-luminance, so a light ink fails on it, 2.1:1). Unselected:
    // the quiet-well style from init() (background fill, colourDisabled
    // border, textDim ink).
    lv_obj_set_style_bg_color(overlayModeTile[i],
                              selected ? m_palette->accent
                                       : m_palette->background, 0);
    lv_obj_set_style_border_color(overlayModeTile[i],
                                  selected ? m_palette->accent
                                           : m_palette->colourDisabled, 0);
    lv_obj_set_style_text_color(overlayModeTileLabel[i],
                                selected ? m_palette->chipInk
                                         : m_palette->textDim, 0);
  }
}

// STOPS. The band-4 travel bar again, at the panel's full width.
//
// The three readouts are taken from the TS_TRAVEL_* caches rather than
// recomputed: drawTravel() has already refreshed them THIS tick (update()
// calls it immediately before drawOverlay(), and says so), so the values are
// current, and reusing them means the overlay cannot disagree with the bar
// underneath it about a stop position or a datum. Recomputing would mean
// duplicating the whole lib/dro datum resolution here, which is exactly the
// kind of second opinion the datum rules must not have.
void Display::drawOverlayStops(int confirmPermille) {
  const bool leftSet = m_leadscrew->getStopPositionState(
                         LeadscrewStopPosition::LEFT) == LeadscrewStopState::SET;
  const bool rightSet = m_leadscrew->getStopPositionState(
                          LeadscrewStopPosition::RIGHT) == LeadscrewStopState::SET;

  setLabelText(overlayStopsLeftLabel, TS_OV_STOP_LEFT,
               m_textCache[TS_TRAVEL_LEFT]);
  setLabelText(overlayStopsRightLabel, TS_OV_STOP_RIGHT,
               m_textCache[TS_TRAVEL_RIGHT]);
  // Value and unit are one label here (the panel has room), so they are joined
  // rather than positioned relative to each other as band 4 does.
  char pos[TEXT_SLOT_LEN];
  snprintf(pos, sizeof(pos), "%s %s", m_textCache[TS_TRAVEL_POS],
           m_textCache[TS_TRAVEL_UNIT]);
  setLabelText(overlayStopsPosLabel, TS_OV_STOP_POS, pos);

  // Set = colourRun, unset = ghost tick, as in drawTravel().
  if (leftSet != m_lastOverlayLeftStopSet) {
    m_lastOverlayLeftStopSet = leftSet;
    lv_obj_set_style_bg_color(overlayStopsLeftMark,
                              leftSet ? m_palette->colourRun
                                      : m_palette->colourDisabled, 0);
  }
  if (rightSet != m_lastOverlayRightStopSet) {
    m_lastOverlayRightStopSet = rightSet;
    lv_obj_set_style_bg_color(overlayStopsRightMark,
                              rightSet ? m_palette->colourRun
                                       : m_palette->colourDisabled, 0);
  }

  bool showCarriage = false;
  int carriageX = m_lastOverlayCarriageX;
  float fraction = 0.0f;
  if (carriageFraction(m_leadscrew, leftSet, rightSet, fraction)) {
    carriageX = OVERLAY_STOP_TRACK_X +
      (int)(fraction * (float)(OVERLAY_STOP_TRACK_W - OVERLAY_STOP_CARRIAGE_W));
    showCarriage = true;
  }
  if (showCarriage && carriageX != m_lastOverlayCarriageX) {
    m_lastOverlayCarriageX = carriageX;
    lv_obj_set_pos(overlayStopsCarriage, carriageX, OVERLAY_STOP_MARK_Y);
  }
  if (showCarriage != m_lastOverlayCarriageShown) {
    m_lastOverlayCarriageShown = showCarriage;
    if (showCarriage) {
      lv_obj_remove_flag(overlayStopsCarriage, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(overlayStopsCarriage, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The clear-both confirm bar. Driven LIVE from the sampled permille every
  // tick: the moment the press ends, stopsConfirmPermille() reads 0 and the
  // whole thing vanishes on the next pass -- releasing IS the cancel, and the
  // bar disappearing is its feedback. Show/hide and the fill width are gated
  // separately, like the carriage above: mid-hold only the width changes, so
  // only the fill repaints.
  const bool confirmShown = confirmPermille > 0;
  if (confirmShown != m_lastStopsConfirmShown) {
    m_lastStopsConfirmShown = confirmShown;
    if (confirmShown) {
      lv_obj_remove_flag(overlayStopsConfirmTrack, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(overlayStopsConfirmFill, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(overlayStopsConfirmLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(overlayStopsConfirmTrack, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(overlayStopsConfirmFill, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(overlayStopsConfirmLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (confirmShown) {
    // Integer scale over the track width; the floor keeps the rounded rect
    // from degenerating in the first fraction of the hold.
    int fillW = (OVERLAY_STOP_TRACK_W * confirmPermille) / 1000;
    if (fillW < OVERLAY_STOP_CONFIRM_FILL_MIN) {
      fillW = OVERLAY_STOP_CONFIRM_FILL_MIN;
    }
    if (fillW != m_lastStopsConfirmFillW) {
      m_lastStopsConfirmFillW = fillW;
      lv_obj_set_size(overlayStopsConfirmFill, fillW, OVERLAY_STOP_CONFIRM_H);
    }
  }
}

// MENU. The tile carousel (docs/ux-redesign.md section 6).
//
// This method RENDERS; it decides nothing. UiState owns the whole interaction -
// menuIndex(), the saturating clamp, what MENU/HALT/the arrows do - and
// MenuTile (lib/ui/uistate.h) owns what each index means. Reimplementing any of
// that here is how the screen and the action dispatcher end up disagreeing
// about which tile is selected.
//
// Neighbours are the WRAPPED index-1 and index+1, so the ring reads as a ring:
// left of the first tile is the last one, and it is right there on screen
// before you press. That is the point of the wrap - you can see where the
// short way round leads.
static_assert(UiState::kMenuItemCount >= 3,
              "the three-across carousel wraps; with fewer than three tiles a "
              "wrapped neighbour would be the selected tile itself");

void Display::drawOverlayMenu(bool motionActive, bool threadMode) {
  if (m_ui == nullptr) {
    return;  // no ButtonPad -> no menu; the Wi-Fi setup path never gets here.
  }
  const int index = m_ui->menuIndex();

  char buf[TEXT_SLOT_LEN];
  // 1-based for the reader, 0-based internally. kMenuItemCount, not a literal 9.
  snprintf(buf, sizeof(buf), "%d / %d", index + 1, UiState::kMenuItemCount);
  setLabelText(overlayMenuPos, TS_OV_MENU_POS, buf);

  // Wrapped, matching UiState's arrow and encoder branches exactly. The
  // + kMenuItemCount before the modulo is what keeps the prev index
  // non-negative at index 0.
  const int prevIndex =
      (index + UiState::kMenuItemCount - 1) % UiState::kMenuItemCount;
  const int nextIndex = (index + 1) % UiState::kMenuItemCount;

  setLabelText(overlayMenuTileLabel[0], TS_OV_MENU_PREV, menuTileName(prevIndex));
  setLabelText(overlayMenuTileLabel[1], TS_OV_MENU_NAME, menuTileName(index));
  setLabelText(overlayMenuTileLabel[2], TS_OV_MENU_NEXT, menuTileName(nextIndex));

  // BOTH neighbours are always present now: the ring wraps, so there is never
  // a direction with nothing beyond it. The hide/show plumbing is kept rather
  // than deleted because the tiles start life hidden or shown according to
  // whatever the previous draw left behind, and this is what reconciles that.
  //
  // Three distinct tiles need at least three entries, or the carousel would
  // show the same tile twice - see the static_assert below.
  const bool prevShown = true;
  const bool nextShown = true;
  if (prevShown != m_lastMenuPrevShown) {
    m_lastMenuPrevShown = prevShown;
    if (prevShown) {
      lv_obj_remove_flag(overlayMenuTile[0], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(overlayMenuTile[0], LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (nextShown != m_lastMenuNextShown) {
    m_lastMenuNextShown = nextShown;
    if (nextShown) {
      lv_obj_remove_flag(overlayMenuTile[2], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(overlayMenuTile[2], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // A tile whose action is unavailable renders UNAVAILABLE, never hidden:
  // dropping it out of the ring would renumber everything after it, so "6 / 9"
  // would name two different tiles depending on the machine's state.
  //
  // Out-of-range neighbours (at the two ends of the list) fall to
  // menuTileBlock()'s default arm and read as MTB_NONE, which is harmless:
  // their whole tile is hidden above, so there is nothing to colour either way.
  //
  // Three looks, all chip-grammar consistent with the MODE tiles:
  //   * centre + live:  accent fill + chipInk    ("the arrows chose THIS")
  //   * blocked (any):  colourDisabled fill + chipInk  (present but refusing;
  //     the hint row says why). A FILL, not caution-coloured text -- caution
  //     text is unreadable on the light surface (1.5:1).
  //   * side + live:    the quiet well from init() (background fill,
  //     colourDisabled border, textDim ink).
  // Only the block state drives a restyle, so arrowing between two live tiles
  // costs no repaint. The two block REASONS share one appearance - the hint
  // row is what tells them apart - so a change of reason costs a (rare,
  // harmless) repaint.
  const MenuTileBlock blocks[3] = {
    menuTileBlock(index - 1, motionActive, threadMode),
    menuTileBlock(index,     motionActive, threadMode),
    menuTileBlock(index + 1, motionActive, threadMode),
  };
  int* lastBlocks[3] = { &m_lastMenuPrevBlock, &m_lastMenuBlock,
                         &m_lastMenuNextBlock };
  for (int i = 0; i < 3; i++) {
    if ((int)blocks[i] == *lastBlocks[i]) {
      continue;
    }
    *lastBlocks[i] = (int)blocks[i];
    const bool live = (blocks[i] == MTB_NONE);
    const bool selected = (i == 1);
    const lv_color_t fill = !live ? m_palette->colourDisabled
                          : selected ? m_palette->accent
                                     : m_palette->background;
    lv_obj_set_style_bg_color(overlayMenuTile[i], fill, 0);
    lv_obj_set_style_border_color(overlayMenuTile[i],
                                  (live && !selected) ? m_palette->colourDisabled
                                                      : fill, 0);
    lv_obj_set_style_text_color(overlayMenuTileLabel[i],
                                (live && !selected) ? m_palette->textDim
                                                    : m_palette->chipInk, 0);
  }
}

void Display::writeLed() {
#ifdef ELS_UI_ENCODER
  int64_t time = micros() / 250000;
  EncoderColour c = time % 2 == 1 ? firstColour : secondColour;
  digitalWrite(ELS_IND_GREEN, (c & 2) == 2);
  digitalWrite(ELS_IND_RED, c & 1);
#endif
}


void Display::updateLed() {
#ifdef ELS_UI_ENCODER

  GlobalState* state = GlobalState::getInstance();
  GlobalMotionMode mode = state->getMotionMode();

  // No lock any more (docs/ux-redesign.md Sec. 7) - these LEDs report motion
  // state only: off when idle, yellow while jogging, green while engaged.
  switch (mode) {
  case GlobalMotionMode::MM_DISABLED:
    firstColour = EC_NONE;
    secondColour = EC_NONE;
    break;
  case GlobalMotionMode::MM_JOG_LEFT:
  case GlobalMotionMode::MM_JOG_RIGHT:
    firstColour = EC_YELLOW;
    secondColour = EC_YELLOW;
    break;
  case GlobalMotionMode::MM_ENABLED:
    firstColour = EC_GREEN;
    secondColour = EC_GREEN;
    break;
  }
#endif

}
#endif
