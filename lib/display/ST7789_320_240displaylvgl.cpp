#include <config.h>

#if ELS_DISPLAY == ST7789_240_135_LVGL
#include <ST7789_320_240displaylvgl.h>
#include <globalstate.h>
#include <dro.h>

#include <stdio.h>
#include <string.h>

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
};

// Dark palette (THEME_DARK, the default). NOTE the naming is historical: these
// are the values the pre-redesign screen rendered with, which is a LIGHT-ground
// look (LVGL's default theme background, black ink). Renaming/retuning the two
// palettes into a genuinely dark one and a light one is a theme-work item, not
// a layout one, so this pass leaves the values alone and only rebuilds the
// layout on top of them.
static const DisplayPalette PALETTE_DARK = {
  lv_color_hex(0xF5F5F5), // background: matches LVGL's implicit default theme
                          // screen colour (LIGHT_COLOR_SCR = lv_palette_
                          // lighten(GREY, 4) in lv_theme_default.c, since
                          // lv_conf.h has LV_THEME_DEFAULT_DARK 0) -- R==B, no
                          // swap needed. init() now paints it explicitly rather
                          // than relying on that default.
  lv_color_hex(0x000000), // textPrimary: was the hardcoded black mode-icon/
                          // RPM ink -- R==B, no swap needed.
  lv_color_hex(0x616161), // textDim: mid grey -- R==B, no swap needed.
  lv_color_hex(0x008800), // colourRun: was COLOUR_GREEN.
  lv_color_hex(0x0084FF), // colourCaution: was COLOUR_YELLOW.
  lv_color_hex(0x0000FF), // colourFault: was COLOUR_RED.
  lv_color_hex(0xCCCCCC), // colourDisabled: was COLOUR_DISABLED.
  lv_color_hex(0xFFFFFF), // iconInk: was the hardcoded white status-icon ink
                          // -- R==B, no swap needed.
};

// Light palette: not reachable yet -- no menu wires LatheConfig::theme to
// THEME_LIGHT, and setTheme() below is not called from anywhere. background/
// textPrimary/textDim are placeholders (unconsumed, same as in dark).
// colourRun/colourCaution/colourFault/colourDisabled come from
// docs/ux-redesign.md section 8 "Colour", which gives them pre-swapped
// already; the swap arithmetic (R<->B byte swap of the doc's natural hex) is
// reproduced here so it's checkable without cross-referencing the doc:
//   run:      natural #00C853 -> R=00,G=C8,B=53 -> swap R/B -> 53,C8,00 -> 0x53C800
//   caution:  natural #FFAB00 -> R=FF,G=AB,B=00 -> swap R/B -> 00,AB,FF -> 0x00ABFF
//   fault:    natural #FF3B30 -> R=FF,G=3B,B=30 -> swap R/B -> 30,3B,FF -> 0x303BFF
//   disabled: natural #6B7280 -> R=6B,G=72,B=80 -> swap R/B -> 80,72,6B -> 0x80726B
// These deliberately do NOT match PALETTE_DARK's state colours even though
// docs/ux-redesign.md section 8 "Theme" says "the accent/state hues are
// shared so only the ground and text tokens differ" -- that line describes an
// eventual full re-skin where dark's state colours would ALSO move to this
// vivid set, but this pass's hard constraint is that dark stays byte-for-byte
// identical to today, so dark keeps today's older/dimmer state hex values
// instead. Worth reconciling when the two palettes are next revisited.
static const DisplayPalette PALETTE_LIGHT = {
  lv_color_hex(0xFFFFFF), // background: white -- R==B, no swap needed.
  lv_color_hex(0x000000), // textPrimary: black -- R==B, no swap needed.
  lv_color_hex(0x757575), // textDim: mid grey -- R==B, no swap needed.
  lv_color_hex(0x53C800), // colourRun, see swap working above.
  lv_color_hex(0x00ABFF), // colourCaution, see swap working above.
  lv_color_hex(0x303BFF), // colourFault, see swap working above.
  lv_color_hex(0x80726B), // colourDisabled, see swap working above.
  // iconInk: BLACK here, unlike dark's white -- R==B, no swap needed.
  //
  // The status icons are recoloured to iconInk and (in the overlays that will
  // use them) drawn ON TOP of the state colours above, so this pairing is what
  // has to be legible, not the ink against the screen background. Dark's are
  // the old dim hues (0x008800 green), which white reads well against. Light's
  // are the design doc's vivid hues, and white on those is washed out --
  // measured contrast is 2.24:1 on colourRun and 1.90:1 on colourCaution, both
  // under the 3:1 minimum for UI glyphs. Black against the same two is roughly
  // 9.8:1 and 12:1.
  //
  // This is the same class of mistake the I1->A8 icon conversion already made
  // once (black ink landing on the green "engaged" chip), so it is spelled out
  // rather than left to be rediscovered.
  lv_color_hex(0x000000),
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
//   124 +------------------------------------------+
//       |  . . . . # . . . . . . . .               |  pitch ticker
//   146 +------------------------------------------+
//       | |--o---------------|                       |
//       | L 0.00        12.40 mm             48.00 R |  carriage travel
//   192 +------------------------------------------+
//       |  * IDLE                                  |  state
//       |     HALT        MENU        RUN          |  soft-key hints
//   240 +------------------------------------------+
//
// Every y below is derived from those boundaries plus the ACTUAL Montserrat
// metrics (line_height / base_line from lvgl's lv_font_montserrat_*.c):
//     size 14: line_height 16, base_line 3  -> ascent 13
//     size 26: line_height 29, base_line 5  -> ascent 24
//     size 48: line_height 52, base_line 9  -> ascent 43
// A single-line label's height IS its font's line_height, so "baseline-align a
// 14 with a 26" means y14 = y26 + (24 - 13) = y26 + 11.
//
// Only Montserrat 14/26/36/48 are compiled in (include/lv_conf.h) -- any other
// size fails to link. This screen uses 14, 26 and 48 only.
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;

static const int BAND_STATUS_BOTTOM = 34;
static const int BAND_PITCH_BOTTOM = 124;
static const int BAND_TICKER_BOTTOM = 146;
static const int BAND_TRAVEL_BOTTOM = 192;

// Band 1 -- status bar, 0..33 (h 34).
// 26 centred: (34-29)/2 = 2. 14 centred: (34-16)/2 = 9.
// "RPM" is baseline-aligned to the value instead: 2 + 11 = 13.
static const int STATUS_CHIP_Y = 9;
static const int STATUS_MODE_X = 8;    // widest text "THREAD R" = 75px -> 83
static const int STATUS_UNIT_X = 96;   // widest text "inch" = 31px -> 127
static const int STATUS_SYNC_X = 146;  // "SYNC" = 39px -> 185
static const int STATUS_RPM_VALUE_Y = 2;
static const int STATUS_RPM_VALUE_X = 186;  // right-aligned box, 186..266;
static const int STATUS_RPM_VALUE_W = 80;   // "9999" @26 = 64px -> ink from 202
static const int STATUS_RPM_UNIT_X = 272;   // "RPM" @14 = 34px -> 306
static const int STATUS_RPM_UNIT_Y = 13;

// Band 2 -- primary readout, 35..123 (h 89).
// 48 centred: 35 + (89-52)/2 = 53. Glyph centred: 35 + (89-64)/2 = 47.
static const int PITCH_VALUE_X = 10;
static const int PITCH_VALUE_Y = 53;
static const int PITCH_UNIT_GAP = 8;
// lv_obj_align_to(OUT_RIGHT_BOTTOM) bottom-aligns the two boxes: that puts the
// 26 at y = 53 + (52-29) = 76, baseline 100, against the 48's baseline 96. The
// -4 offset pulls it back onto the same baseline.
static const int PITCH_UNIT_BASELINE_FIX = -4;
static const int MODE_GLYPH_X = 188;  // 128 wide -> 188..315
static const int MODE_GLYPH_Y = 47;   // 64 tall  -> 47..110

// Band 3 -- pitch ticker, 125..145 (h 21). 8px track centred: 125+(21-8)/2 = 131.
static const int TICKER_X = 12;
static const int TICKER_Y = 131;
static const int TICKER_W = 296;
static const int TICKER_H = 8;
static const int TICKER_KNOB_PAD = 3;  // -> a 14x14 marker, 128..141

// Band 4 -- carriage travel, 147..191 (h 45).
static const int TRAVEL_TRACK_X = 12;
static const int TRAVEL_TRACK_Y = 150;
static const int TRAVEL_TRACK_W = 296;  // 12..307
static const int TRAVEL_TRACK_H = 8;
static const int TRAVEL_MARK_Y = 147;   // 14 tall -> 147..160, clear of the rule
static const int TRAVEL_MARK_H = 14;
static const int TRAVEL_MARK_W = 4;
static const int TRAVEL_CARRIAGE_W = 8;
// 26 at y 161 -> box 161..189, baseline 185. The 14s sit at 185-13 = 172.
static const int TRAVEL_VALUE_Y = 161;
static const int TRAVEL_LABEL_Y = 172;
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

// Band 5 -- state + soft keys, 193..239 (h 47).
// Two rows: the 26 state word (29 tall) then the 14 hint row (16 tall) = 45.
static const int STATE_DOT_X = 10;
static const int STATE_DOT_Y = 203;
static const int STATE_DOT_SIZE = 12;
static const int STATE_WORD_X = 28;
static const int STATE_WORD_Y = 194;   // box 194..222
static const int STATE_WORD_W = 170;   // "RETURNING" @26 = 161px, clipped beyond
static const int SOFTKEY_Y = 223;      // box 223..238
static const int SOFTKEY_W = 106;      // three columns mirroring the 3x3 keypad
static const int SOFTKEY_X0 = 0;       // 0..105, 106..211, 212..317

// Font box heights, for the layout assertions below (see the metrics table
// above): a single-line label is exactly line_height tall.
static const int FONT14_H = 16;
static const int FONT26_H = 29;
static const int FONT48_H = 52;
static const int GLYPH_W = 128;
static const int GLYPH_H = 64;

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
static const int TEXT26_STATE_W = 161;     // "RETURNING" (longest state word)

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
// them against the measured ink, not just against each other. sync -> RPM is
// the tightest gap on the whole screen (185 vs 186).
static_assert(STATUS_MODE_X + TEXT14_MODE_W <= STATUS_UNIT_X, "mode text runs into the unit chip");
static_assert(STATUS_UNIT_X + TEXT14_UNIT_W <= STATUS_SYNC_X, "unit chip runs into the sync chip");
static_assert(STATUS_SYNC_X + TEXT14_SYNC_W <= STATUS_RPM_VALUE_X, "sync chip runs into the RPM box");
static_assert(STATUS_RPM_UNIT_X + TEXT14_RPM_UNIT_W <= SCREEN_W, "RPM unit text off the right edge");
static_assert(TEXT26_RPM_VALUE_W <= STATUS_RPM_VALUE_W, "RPM value wider than its box");
// Band 2
static_assert(PITCH_VALUE_Y > BAND_STATUS_BOTTOM, "pitch value overlaps band 1");
static_assert(PITCH_VALUE_Y + FONT48_H <= BAND_PITCH_BOTTOM, "pitch value overflows band 2");
static_assert(MODE_GLYPH_Y > BAND_STATUS_BOTTOM, "mode glyph overlaps band 1");
static_assert(MODE_GLYPH_Y + GLYPH_H <= BAND_PITCH_BOTTOM, "mode glyph overflows band 2");
static_assert(MODE_GLYPH_X + GLYPH_W <= SCREEN_W, "mode glyph off the right edge");
// Band 3 -- the knob overhangs the track by TICKER_KNOB_PAD at top and bottom.
static_assert(TICKER_Y - TICKER_KNOB_PAD > BAND_PITCH_BOTTOM, "ticker knob overlaps band 2");
static_assert(TICKER_Y + TICKER_H + TICKER_KNOB_PAD <= BAND_TICKER_BOTTOM, "ticker knob overflows band 3");
static_assert(TICKER_X + TICKER_W <= SCREEN_W, "ticker off the right edge");
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
// Band 5 -- two rows: the state word, then the soft-key hints.
static_assert(STATE_WORD_Y > BAND_TRAVEL_BOTTOM, "state word overlaps band 4");
static_assert(STATE_WORD_Y + FONT26_H <= SOFTKEY_Y, "state word overlaps the hint row");
static_assert(STATE_WORD_X + STATE_WORD_W <= SCREEN_W, "state word box off the right edge");
static_assert(TEXT26_STATE_W <= STATE_WORD_W, "state word wider than its box");
static_assert(STATE_DOT_Y + STATE_DOT_SIZE <= SOFTKEY_Y, "state dot overlaps the hint row");
static_assert(STATE_DOT_X + STATE_DOT_SIZE < STATE_WORD_X, "state dot collides with the word");
static_assert(SOFTKEY_Y + FONT14_H <= SCREEN_H, "soft-key hints off the bottom");
static_assert(SOFTKEY_X0 + (3 * SOFTKEY_W) <= SCREEN_W, "soft-key columns off the right edge");

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

Display::Display(Spindle* spindle, Leadscrew* leadscrew) {
  this->m_spindle = spindle;
  this->m_leadscrew = leadscrew;
  this->m_globalState = GlobalState::getInstance();
  // Theme is picked once here, not re-read from config on every init() rebuild
  // (Display::update() calls init() whenever getDisplayReset() fires) -- if it
  // were re-read every time, a future runtime setTheme() call would just get
  // clobbered back to the config default on the very next rebuild it triggers.
  // Any value other than THEME_LIGHT falls back to dark, mirroring the same
  // safe-fallback pattern latheconfig.cpp uses for droDatum.
  uint8_t theme = (leadscrew != nullptr) ? leadscrew->getConfig()->theme() : THEME_DARK;
  this->m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
  // Owned by initDisplay(), which is the only writer and runs before any read
  // of either -- but CLAUDE.md's rule is every member, and these two are the
  // ones the class was missing (Display is `new`ed, so they are heap garbage
  // until then, and a stray read would be a wild pointer rather than a crash).
  this->disp = nullptr;
  this->draw_buf = nullptr;
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
  this->m_globalState = GlobalState::getInstance();
  this->m_palette = &PALETTE_DARK;  // no config available on this path -- default dark.
  this->disp = nullptr;             // see the other constructor.
  this->draw_buf = nullptr;
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
  pitchSlider = nullptr;
  travelTrack = nullptr;
  travelLeftMark = nullptr;
  travelRightMark = nullptr;
  travelCarriage = nullptr;
  travelLeftLabel = nullptr;
  travelRightLabel = nullptr;
  travelPosLabel = nullptr;
  travelPosUnit = nullptr;
  stateDot = nullptr;
  stateLabel = nullptr;
  for (int i = 0; i < 3; i++) {
    softKeyLabel[i] = nullptr;
  }
  for (int i = 0; i < 4; i++) {
    bandRule[i] = nullptr;
  }
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

// Runtime theme switch. Not wired to any UI yet -- docs/ux-redesign.md
// section 8 "Theme" menu tile doesn't exist -- but reachable: re-points
// m_palette and asks Display::update() to rebuild the whole screen from
// scratch next tick via the existing getDisplayReset()/init() path
// (see Display::update() below), the same mechanism already used for the
// OTA <-> normal screen swap, so no new plumbing is needed.
void Display::setTheme(uint8_t theme) {
  m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
  m_globalState->setDisplayReset();
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

void Display::showWifi(const char* ssid, const char* password, IPAddress ip) {
  initDisplay();

  // Left column: credentials as text (fallback if the QR can't be scanned).
  lv_obj_t* ssidLabel = lv_label_create(lv_screen_active());
  lv_obj_t* passwordLabel = lv_label_create(lv_screen_active());
  lv_obj_t* ipLabel = lv_label_create(lv_screen_active());

  lv_obj_t* ssidText = lv_label_create(lv_screen_active());
  lv_obj_t* passwordText = lv_label_create(lv_screen_active());
  lv_obj_t* ipText = lv_label_create(lv_screen_active());

  lv_obj_set_style_text_font(ssidLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(passwordLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(ipLabel, &lv_font_montserrat_14, 0);

  lv_obj_set_style_text_font(ssidText, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_font(passwordText, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_font(ipText, &lv_font_montserrat_26, 0);

  lv_obj_set_pos(ssidLabel, 10, 12);
  lv_obj_set_pos(ssidText, 10, 28);
  lv_obj_set_pos(passwordLabel, 10, 82);
  lv_obj_set_pos(passwordText, 10, 98);
  lv_obj_set_pos(ipLabel, 10, 152);
  lv_obj_set_pos(ipText, 10, 168);

  lv_label_set_text(ssidLabel, "Wifi SSID");
  lv_label_set_text(passwordLabel, "Password");
  lv_label_set_text(ipLabel, "IP Address");

  lv_label_set_text(ssidText, ssid);
  lv_label_set_text(passwordText, password);
  lv_label_set_text(ipText, ip.toString().c_str());

  // Right side: a Wi-Fi join QR code. Scanning it on a phone connects straight
  // to the setup AP (the captive portal then opens the config page).
  lv_obj_t* scanLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(scanLabel, &lv_font_montserrat_14, 0);
  lv_label_set_text(scanLabel, "Scan to join");
  lv_obj_set_pos(scanLabel, 196, 24);

  String qrPayload = "WIFI:S:";
  appendWifiQrEscaped(qrPayload, ssid);
  qrPayload += ";T:WPA;P:";
  appendWifiQrEscaped(qrPayload, password);
  qrPayload += ";;";

  lv_obj_t* wifiQr = lv_qrcode_create(lv_screen_active());
  lv_qrcode_set_size(wifiQr, 124);
  lv_qrcode_set_dark_color(wifiQr, lv_color_black());
  lv_qrcode_set_light_color(wifiQr, lv_color_white());
  // White background + padding gives the quiet zone scanners need.
  lv_obj_set_style_bg_color(wifiQr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(wifiQr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(wifiQr, 0, 0);
  lv_obj_set_style_pad_all(wifiQr, 6, 0);
  lv_qrcode_update(wifiQr, qrPayload.c_str(), qrPayload.length());
  lv_obj_set_pos(wifiQr, 182, 46);

  lv_timer_handler();

}

// Shown once a device has joined the setup AP. Deliberately NO QR: on phones the
// OS routes the plain browser over cellular while the AP is "captive", so a
// browser QR would mislead users into a route that won't work. Phones should use
// the OS "Sign in to network" prompt; the IP is shown for computers on the AP.
void Display::showConnected(IPAddress ip) {
  // Replace the join screen (LVGL is already initialised by showWifi()).
  lv_obj_clean(lv_screen_active());

  lv_obj_t* title = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
  lv_label_set_text(title, "Connected!");
  lv_obj_set_pos(title, 10, 14);

  lv_obj_t* msg = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
  lv_label_set_text(msg,
    "A device joined the setup network.\n\n"
    "On your phone, tap the\n"
    "\"Sign in to network\" prompt\n"
    "to open the configuration page.");
  lv_obj_set_pos(msg, 10, 72);

  lv_obj_t* ipLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(ipLabel, &lv_font_montserrat_14, 0);
  lv_label_set_text(ipLabel, "On a computer, browse to:");
  lv_obj_set_pos(ipLabel, 10, 178);

  lv_obj_t* ipText = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(ipText, &lv_font_montserrat_26, 0);
  lv_label_set_text(ipText, ip.toString().c_str());
  lv_obj_set_pos(ipText, 10, 198);

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

  updateLabel = lv_label_create(lv_screen_active());
  lv_label_set_text(updateLabel, "Updating...");
  lv_obj_set_style_text_font(updateLabel, &lv_font_montserrat_26, 0);
  lv_obj_align(updateLabel, LV_ALIGN_CENTER, 0, 0);

  updateSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(updateSlider, 280, 10);
  lv_obj_set_pos(updateSlider, 20, 150);

  lv_obj_set_style_opa(updateSlider, LV_OPA_0, LV_PART_KNOB);

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
  // Static text; only its colour tracks GlobalThreadSyncState (drawStatusBar).
  syncLabel = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                          m_palette->colourDisabled, STATUS_SYNC_X, STATUS_CHIP_Y);
  lv_label_set_text(syncLabel, "SYNC");

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
  lv_obj_set_style_image_recolor(feedSymbolObj, m_palette->textPrimary, 0);
  lv_obj_set_style_image_recolor_opa(feedSymbolObj, LV_OPA_COVER, 0);

  // --- band 3: pitch ticker -------------------------------------------------
  // The existing slider, restyled and repositioned: it already tracks the
  // position within the current pitch list. The INDICATOR is painted the same
  // colour as the track so it reads as a marker on a scale (a ticker) rather
  // than a fill level -- the knob is the "you are here".
  pitchSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(pitchSlider, TICKER_W, TICKER_H);
  lv_obj_set_pos(pitchSlider, TICKER_X, TICKER_Y);
  lv_obj_set_style_pad_all(pitchSlider, 0, 0);
  lv_obj_set_style_bg_color(pitchSlider, m_palette->colourDisabled, LV_PART_MAIN);
  lv_obj_set_style_radius(pitchSlider, RADIUS_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pitchSlider, m_palette->colourDisabled, LV_PART_INDICATOR);
  lv_obj_set_style_radius(pitchSlider, RADIUS_TRACK, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(pitchSlider, m_palette->textPrimary, LV_PART_KNOB);
  lv_obj_set_style_radius(pitchSlider, RADIUS_TRACK, LV_PART_KNOB);
  lv_obj_set_style_pad_all(pitchSlider, TICKER_KNOB_PAD, LV_PART_KNOB);

  // --- band 4: carriage travel ---------------------------------------------
  travelTrack = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_TRACK_Y,
                           TRAVEL_TRACK_W, TRAVEL_TRACK_H,
                           m_palette->colourDisabled, RADIUS_TRACK);
  // Markers and the carriage are siblings of the track, not children of it: a
  // child would be clipped to the 8px-high track, and these are 14 tall.
  travelLeftMark = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_MARK_Y,
                              TRAVEL_MARK_W, TRAVEL_MARK_H,
                              m_palette->colourRun, 0);
  travelRightMark = createRect(lv_screen_active(),
                               TRAVEL_TRACK_X + TRAVEL_TRACK_W - TRAVEL_MARK_W,
                               TRAVEL_MARK_Y, TRAVEL_MARK_W, TRAVEL_MARK_H,
                               m_palette->colourRun, 0);
  travelCarriage = createRect(lv_screen_active(), TRAVEL_TRACK_X, TRAVEL_MARK_Y,
                              TRAVEL_CARRIAGE_W, TRAVEL_MARK_H,
                              m_palette->textPrimary, RADIUS_TRACK);
  lv_obj_add_flag(travelLeftMark, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(travelRightMark, LV_OBJ_FLAG_HIDDEN);
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

  // --- band 5: state + soft keys -------------------------------------------
  stateDot = createRect(lv_screen_active(), STATE_DOT_X, STATE_DOT_Y,
                        STATE_DOT_SIZE, STATE_DOT_SIZE,
                        m_palette->colourDisabled, LV_RADIUS_CIRCLE);
  stateLabel = createLabel(lv_screen_active(), &lv_font_montserrat_26,
                           m_palette->colourDisabled, STATE_WORD_X, STATE_WORD_Y);
  fixLabelBox(stateLabel, STATE_WORD_W, LV_TEXT_ALIGN_LEFT);

  // Static hints mirroring the bottom physical row of the 3x3 keypad
  // (docs/ux-redesign.md section 2: HALT | MENU | ENABLE), one per column, so
  // the panel documents the two keys that have no on-screen state of their own.
  // Only the third changes (RUN <-> STOP, drawStateBar()).
  const char* hints[3] = { "HALT", "MENU", "RUN" };
  for (int i = 0; i < 3; i++) {
    softKeyLabel[i] = createLabel(lv_screen_active(), &lv_font_montserrat_14,
                                  m_palette->textDim,
                                  SOFTKEY_X0 + (i * SOFTKEY_W), SOFTKEY_Y);
    fixLabelBox(softKeyLabel[i], SOFTKEY_W, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(softKeyLabel[i], hints[i]);
  }
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
  }
  writeLed();
}

void Display::drawOTA() {
  GlobalState* state = GlobalState::getInstance();
  switch (state->getOtaStatus()) {
  case OTA_CHECKING:
  case OTA_IDLE:
    // OTA_IDLE is the brief window before the OTA task sets CHECKING.
    lv_label_set_text(updateLabel, "Checking for updates...");
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
    lv_obj_set_style_text_color(
      syncLabel,
      sync == SS_SYNC ? m_palette->colourRun : m_palette->colourDisabled, 0);
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
  case FM_JOG:
    src = &jog;
    break;
  case FM_THREAD:
    // Right-hand thread: upright helix.
    src = &threadSymbol;
    break;
  case FM_THREAD_REVERSE:
    // Left-hand / reverse thread: vertically-flipped helix (a pre-flipped
    // asset -- this LVGL has no image-flip API).
    src = &threadSymbolReverse;
    break;
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
// imperial; feed pitch reads mm metric / THOU imperial; jog reads %.
//
// The pitch tables are indexed directly here, exactly as before. It is NOT
// legitimate to render GlobalState::getCurrentFeedPitch() instead: that returns
// mm/rev in every mode (so imperial would show the metric equivalent) and it
// negates for FM_THREAD_REVERSE. Note also that feedPitchImperial[] is
// commented thou/rev but actually holds INCHES -- the `* 1000` below is what
// makes this readout correct, and it is deliberately left in place; the array's
// mislabelling (and getCurrentFeedPitch()'s consequent 1000x error) is tracked
// separately and must not be "fixed" from here.
void Display::drawPitch() {
  const GlobalUnitMode unit = m_globalState->getUnitMode();
  const GlobalFeedMode mode = m_globalState->getFeedMode();
  const bool thread = (mode == FM_THREAD || mode == FM_THREAD_REVERSE);
  const int feedSelect = m_globalState->getFeedSelect();

  char value[TEXT_SLOT_LEN];
  const char* unitText;
  int tickerCount;
  int tickerIndex;

  if (mode == FM_JOG) {
    snprintf(value, sizeof(value), "%d", (int)(m_globalState->getJogSpeed() * 100));
    unitText = "%";
    tickerCount = (int)ARRAY_SIZE(jogSpeeds);
    tickerIndex = m_globalState->getJogIndex();
  } else if (unit == METRIC) {
    snprintf(value, sizeof(value), "%.2f",
             (double)(thread ? threadPitchMetric[feedSelect]
                             : feedPitchMetric[feedSelect]));
    unitText = "mm";
    tickerCount = thread ? (int)ARRAY_SIZE(threadPitchMetric)
                         : (int)ARRAY_SIZE(feedPitchMetric);
    tickerIndex = feedSelect;
  } else if (thread) {
    snprintf(value, sizeof(value), "%d", (int)threadPitchImperial[feedSelect]);
    unitText = "TPI";
    tickerCount = (int)ARRAY_SIZE(threadPitchImperial);
    tickerIndex = feedSelect;
  } else {
    snprintf(value, sizeof(value), "%d",
             (int)(feedPitchImperial[feedSelect] * 1000));
    unitText = "thou";
    tickerCount = (int)ARRAY_SIZE(feedPitchImperial);
    tickerIndex = feedSelect;
  }

  const bool valueChanged = setLabelText(pitchLabel, TS_PITCH, value);
  setLabelText(pitchUnitLabel, TS_PITCH_UNIT, unitText);
  // The unit hangs off the right-hand edge of the value, so it only has to move
  // when the VALUE's width changes. Doing this unconditionally would re-position
  // (and so invalidate) it on every one of the 10 ticks a second.
  if (valueChanged) {
    lv_obj_align_to(pitchUnitLabel, pitchLabel, LV_ALIGN_OUT_RIGHT_BOTTOM,
                    PITCH_UNIT_GAP, PITCH_UNIT_BASELINE_FIX);
  }

  // lv_bar_set_value/range compare before acting, so these are free when
  // nothing has changed. Range is 0..count-1 against a 0-based index.
  lv_slider_set_min_value(pitchSlider, 0);
  lv_slider_set_max_value(pitchSlider, tickerCount > 0 ? tickerCount - 1 : 0);
  lv_slider_set_value(pitchSlider, tickerIndex, LV_ANIM_OFF);
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
  dro.preference = cfg->droDatum();

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

  // Stop markers: shown only for a stop that is actually set.
  if (leftSet != m_lastLeftStopSet) {
    m_lastLeftStopSet = leftSet;
    if (leftSet) {
      lv_obj_remove_flag(travelLeftMark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(travelLeftMark, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (rightSet != m_lastRightStopSet) {
    m_lastRightStopSet = rightSet;
    if (rightSet) {
      lv_obj_remove_flag(travelRightMark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(travelRightMark, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The carriage marker needs a SPAN to sit in, and only two set stops provide
  // one -- with a single stop (or none) there is no scale, and parking the
  // marker anywhere would be a made-up reading. It is hidden in that case; the
  // numeric readout still shows where the carriage is. Position increases to
  // the right (LeadscrewDirection::RIGHT = 1), so a non-positive span means the
  // stops are the wrong way round and is treated the same way.
  bool showCarriage = false;
  int carriageX = m_lastCarriageX;
  if (leftSet && rightSet) {
    const float lo = m_leadscrew->getStopPositionMM(LeadscrewStopPosition::LEFT);
    const float hi = m_leadscrew->getStopPositionMM(LeadscrewStopPosition::RIGHT);
    const float span = hi - lo;
    if (span > 0.0f) {
      float fraction = (m_leadscrew->getPositionMM() - lo) / span;
      if (fraction < 0.0f) {
        fraction = 0.0f;
      } else if (fraction > 1.0f) {
        fraction = 1.0f;
      }
      carriageX = TRAVEL_TRACK_X +
        (int)(fraction * (float)(TRAVEL_TRACK_W - TRAVEL_CARRIAGE_W));
      showCarriage = true;
    }
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

// Band 5 -- the machine state word and the soft-key hints.
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
    lv_obj_set_style_text_color(stateLabel, colour, 0);
    lv_obj_set_style_bg_color(stateDot, colour, 0);
  }

  // The third soft key mirrors the physical ENABLE key and reflects what it
  // will do next.
  setLabelText(softKeyLabel[2], TS_SOFTKEY, mode == MM_ENABLED ? "STOP" : "RUN");

  updateLed();
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
  GlobalButtonLock lock = GlobalState::getInstance()->getButtonLock();

  switch (mode) {
  case GlobalMotionMode::MM_DISABLED:
    firstColour = lock == LK_LOCKED ? EC_RED : EC_NONE;
    secondColour = lock == LK_LOCKED ? EC_RED : EC_NONE;
    break;
  case GlobalMotionMode::MM_JOG_LEFT:
  case GlobalMotionMode::MM_JOG_RIGHT:
    firstColour = EC_YELLOW;
    secondColour = EC_YELLOW;
    break;
  case GlobalMotionMode::MM_ENABLED:
    firstColour = lock == LK_LOCKED ? EC_RED : EC_GREEN;
    secondColour = EC_GREEN;
    break;
  }
#endif

}
#endif
