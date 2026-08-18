
#include <config.h>
#include <globalstate.h>
#include <leadscrew.h>
#include <spindle.h>
// The focus model (lib/ui). Read-only here: the display renders whichever
// selector overlay UiState says has focus, it never drives it. ButtonPad owns
// the UiState value and BOTH run on the DisplayTask (main.cpp), so this is a
// same-task read and needs no volatile / GlobalState round trip.
#include <uistate.h>


#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>

LV_IMAGE_DECLARE(feedSymbol);
LV_IMAGE_DECLARE(threadSymbol);
LV_IMAGE_DECLARE(threadSymbolReverse);
// These three are the only images left. The eight 32x32 status icons
// (leftstop/rightstop/locked/unlocked/left/right/pause/sync) and the 128x64
// jog glyph are gone: the chips they filled were replaced in the redesign
// (docs/ux-redesign.md section 8) by text and colour in the status bar, the
// carriage-travel band and the state bar, and FM_JOG left the mode cycle
// (section 3), so none of them had a consumer. The three above are generated
// by scripts/make_glyphs.py -- edit that, not the .c files.

#define DRAW_BUF_SIZE ((TFT_WIDTH * TFT_HEIGHT / 10) * (LV_COLOR_DEPTH / 8))

// --- The menu tiles (docs/ux-redesign.md section 6, "MENU") -------------------
//
// enum MenuTile, enum MenuTileBlock and menuTileBlock() (the availability rule)
// moved to lib/ui/uistate.h - pulled in above via <uistate.h>. They live there,
// not here, because menuTileBlock() is safety-relevant motion-gating logic that
// must be covered by the native host tests, which cannot build anything behind
// <lvgl.h>. This file remains one of the rule's two consumers: it renders the
// carousel from MenuTile/menuTileBlock() (Display::drawOverlayMenu() /
// drawOverlay()); src/buttonpad.cpp is the other (ButtonPad::activateMenuTile()).
// See lib/ui/uistate.h for the full doc comment on both.

// Runtime colour palette (dark/light). Full definition + the two compiled-in
// instances (PALETTE_DARK, PALETTE_LIGHT) live in the .cpp -- only a pointer
// to the active one is needed here. See the struct's own doc comment there
// for the colour-order (R<->B swap) rules before touching either instance.
struct DisplayPalette;

class Display {
private:
  bool initOta = false;
  Spindle* m_spindle;
  Leadscrew* m_leadscrew;
  GlobalState* m_globalState;
  // Focus state, owned by ButtonPad (main.cpp constructs it first and passes
  // &keyPad->ui() here). nullptr on the Wi-Fi-setup path, where no ButtonPad
  // exists -- every overlay path must tolerate that.
  const UiState* m_ui;
  const DisplayPalette* m_palette;  // selected in the constructor (see .cpp);
                                     // re-pointed at runtime only by setTheme().
  // Live DRO datum preference. Seeded from LatheConfig in the constructor and
  // changed at runtime by setDroDatum() (the "DRO datum" menu tile), exactly as
  // m_palette is by setTheme(), and for the same reason: LatheConfigDerived
  // hands out a READ-ONLY view (getConfig()) and keeps its LatheConfig* private,
  // so a menu toggle cannot write through to the struct drawTravel() would
  // otherwise read. Without this the datum would only move on the next reboot.
  // This is therefore the authority for the datum while the machine is running;
  // the flash copy is the authority at boot. Deliberately NOT touched by
  // resetObjectTree(), like m_palette - a screen rebuild must not revert it.
  DroDatumPreference m_droDatum;
#ifdef ELS_UI_ENCODER
  EncoderColour firstColour = EC_NONE;
  EncoderColour secondColour = EC_NONE;
#endif
  bool updating = false;
  lv_display_t* disp;
  uint32_t* draw_buf;
  bool initialised = false;

  // --- Main-screen object tree (docs/ux-redesign.md section 8 layout) --------
  // Built ONCE in init(); the draw*() methods only push values into it. Grouped
  // by the horizontal band they live in; band boundaries are the LAYOUT_*
  // constants in the .cpp.

  // Band 1, status bar (y 0..33)
  lv_obj_t* modeLabel;      // FEED / THREAD R / THREAD L / JOG
  lv_obj_t* unitLabel;      // mm / inch
  lv_obj_t* syncLabel;      // "SYNC", coloured by GlobalThreadSyncState
  lv_obj_t* rpmLabel;       // spindle RPM value, right-aligned
  lv_obj_t* rpmUnitLabel;   // "RPM"

  // Band 2, primary readout (y 35..123)
  lv_obj_t* pitchLabel;      // pitch value (Montserrat 48)
  lv_obj_t* pitchUnitLabel;  // mm / TPI / thou / % (Montserrat 26)
  lv_obj_t* feedSymbolObj;   // 128x64 mode glyph

  // Band 3, the pitch pip row. NOT a slider: the pitch list is a discrete set
  // of values, so the band shows one pip per entry -- current tall + accent,
  // its neighbours mid-height + textDim, the rest short + colourDisabled --
  // rather than a continuous track that misreads as a second travel bar.
  // PIP_MAX rects are built once in init(); drawPitch() positions the first
  // `count` of them (the table length varies by mode/unit: the four pitch
  // tables hold 20, jogSpeeds holds 6) and hides the surplus. Reflow happens
  // only when the COUNT changes, restyle only when the INDEX changes.
  enum { PIP_MAX = 20 };
  lv_obj_t* pitchPips[PIP_MAX];

  // Band 4, carriage travel -- the small DRO (y 147..191)
  lv_obj_t* travelTrack;      // the full-width track
  lv_obj_t* travelLeftMark;   // left stop marker (hidden when unset)
  lv_obj_t* travelRightMark;  // right stop marker (hidden when unset)
  lv_obj_t* travelCarriage;   // live carriage position on the track
  lv_obj_t* travelLeftLabel;  // "L <pos>" / "L --"
  lv_obj_t* travelRightLabel; // "<pos> R" / "-- R"
  lv_obj_t* travelPosLabel;   // live carriage position, datum-relative
  lv_obj_t* travelPosUnit;    // mm / in

  // Band 5, state chip (y 197..239). The soft-key hint row is gone -- the
  // physical caps are properly labelled, so the panel no longer mirrors them --
  // and the whole band belongs to the machine-state readout: one Montserrat-36
  // label with a state-coloured chip fill (the label carries its own bg style,
  // so there is no separate dot/fill object to keep in step with it).
  lv_obj_t* stateLabel;

  lv_obj_t* bandRule[4];      // the four band separators

  // --- Selector overlay (docs/ux-redesign.md section 4) ----------------------
  // ONE panel, built once in init() alongside the main screen and left hidden.
  // Its four content groups are swapped by drawOverlay() as focus moves; the
  // panel is never rebuilt or deleted (a rebuild at 10 Hz would be wasteful and
  // would defeat the redraw caches below). It is created LAST so it sits on top
  // of the main screen in the sibling z-order, and it is opaque, so whatever it
  // covers simply is not visible -- LV_DRAW_LAYER_SIMPLE_BUF_SIZE is 24 KB and a
  // translucent panel this size needs ~84 KB (section 8).
  //
  // UiFocus::Menu shares it: the carousel is group D below.
  lv_obj_t* overlayPanel;      // the solid ground + accent border
  lv_obj_t* overlayTitle;      // PITCH / JOG SPEED / MODE / STOPS / MENU
  lv_obj_t* overlayHintLeft;   // what the arrows do
  lv_obj_t* overlayHintRight;  // "OK done" / "OK open"

  // Group A -- the horizontal ticker, shared by Rate and JogSpeed (section 4
  // gives them the same shape: value large, neighbours dimmed either side).
  lv_obj_t* overlayTickerGroup;
  lv_obj_t* overlayTickerPrev;   // neighbour below the current value (26, dim)
  lv_obj_t* overlayTickerValue;  // current value (48)
  lv_obj_t* overlayTickerNext;   // neighbour above (26, dim)
  lv_obj_t* overlayTickerUnit;   // mm / TPI / thou / %

  // Group B -- MODE, three tiles in a row; the current one is filled accent.
  lv_obj_t* overlayModeGroup;
  lv_obj_t* overlayModeTile[3];
  lv_obj_t* overlayModeTileLabel[3];

  // Group C -- STOPS, the travel bar again (full width of the panel) with both
  // markers and the live carriage.
  lv_obj_t* overlayStopsGroup;
  lv_obj_t* overlayStopsTrack;
  lv_obj_t* overlayStopsLeftMark;
  lv_obj_t* overlayStopsRightMark;
  lv_obj_t* overlayStopsCarriage;
  lv_obj_t* overlayStopsLeftLabel;
  lv_obj_t* overlayStopsRightLabel;
  lv_obj_t* overlayStopsPosLabel;
  // The clear-both confirm bar (docs/ux-redesign.md section 4: "STOPS hold -
  // clear both, after a 1 s confirm bar"). Three objects, all hidden at rest
  // and shown only while UiState::stopsConfirmPermille() is non-zero: a
  // full-width colourDisabled track, a colourFault fill that grows across it
  // with the hold, and a static "CLEARING BOTH STOPS" label in chipInk on top
  // -- the words ride ON the bar so the fill and the meaning cannot separate.
  lv_obj_t* overlayStopsConfirmTrack;
  lv_obj_t* overlayStopsConfirmFill;
  lv_obj_t* overlayStopsConfirmLabel;

  // Group D -- MENU, the tile carousel (docs/ux-redesign.md section 6). A
  // carousel and not a list because the panel has only left/right keys: THREE
  // tiles in a row, exactly like the mockup and the MODE widget -- previous /
  // current / next -- with the current (centre) one marked by the accent FILL,
  // not by size. Names are Montserrat 14 and wrap to two lines inside their
  // tile ("Software / update"); the title row carries "n / 9" so the position
  // in a nine-long ring is readable without counting. Labels are CHILDREN of
  // their tiles (unlike the MODE labels): the tile clips them and LVGL keeps
  // them centred as the wrapped line count changes.
  lv_obj_t* overlayMenuGroup;
  lv_obj_t* overlayMenuPos;          // "4 / 9", right of the MENU title
  lv_obj_t* overlayMenuTile[3];      // 0 = prev, 1 = current, 2 = next
  lv_obj_t* overlayMenuTileLabel[3];

  // Group E -- DRO DATUM (docs/ux-redesign.md section 8, "The DRO datum"): a
  // two-choice picker on the same tile grammar as MODE, so the two read as one
  // family. Selection renders the PERSISTED datum -- m_droDatum is the single
  // source of truth (the arrows apply live through setDroDatum(); UiState holds
  // no pending copy). Each tile carries a miniature travel bar with a zero-post
  // at its end of travel, because the travel-bar metaphor is already
  // established on the rest screen and "which end reads 0" is exactly what the
  // setting decides. Bar and post are CHILDREN of their tile (index 0 = LEFT,
  // 1 = RIGHT), so the selection restyle recolours them with the tile.
  lv_obj_t* overlayDatumGroup;
  lv_obj_t* overlayDatumTile[2];
  lv_obj_t* overlayDatumTileLabel[2];
  lv_obj_t* overlayDatumBar[2];   // the mini travel track inside each tile
  lv_obj_t* overlayDatumZero[2];  // the zero-post at that tile's end of travel

  // --- Diagnostics, a full read-only SCREEN (UiFocus::Diagnostics) -----------
  // Not an overlay: it exists to be WATCHED while the machine runs (no idle
  // timeout -- see the ruling on the UiFocus enum in uistate.h), so it takes
  // the whole panel and is built as an instrument: position error dominant
  // with a centre-zero deflection bar under it, spindle / carriage / expected
  // rates in three columns so a ratio problem reads as a mismatch, and the
  // helix sync state as a chip. Static labels (titles, units, hints) are
  // created in init() without members; only the mutated objects live here.
  lv_obj_t* diagPanel;        // full-screen opaque ground; hidden at rest
  // The title row's left half. Reads "DIAGNOSTICS" normally, and the
  // motion-trace capture's state ("REC 812/2400", "FULL: STOP TO SEND",
  // "SENDING TRACE", "SENT 2400") whenever a capture exists - the operator
  // has to be able to see that the instrument is running, and this screen is
  // where the Debug capture tile leaves them. Wording and length are decided
  // by formatCaptureStatus() (lib/global_state/debugcapture.h), which the
  // host tests hold to this row's 20-byte cache slot.
  lv_obj_t* diagTitle;
  lv_obj_t* diagErrValue;     // position error in mm (48)
  lv_obj_t* diagErrPulses;    // the same error in raw pulses (14)
  lv_obj_t* diagErrMarker;    // deflection marker on the centre-zero bar
  lv_obj_t* diagSpindleValue; // spindle RPM (26)
  lv_obj_t* diagCarriageValue;// measured carriage mm/s (26)
  lv_obj_t* diagExpectValue;  // RPM x pitch expectation, "--" unless engaged
  lv_obj_t* diagSyncChip;     // SYNCED / NOT SYNCED, status-bar chip grammar
  lv_obj_t* diagAnchorValue;  // the sync anchor's SOURCE (L stop / R stop /
                              // manual / none), from getSyncAnchorState() --
                              // the bit GlobalThreadSyncState doesn't carry

  // --- About, a full read-only screen (UiFocus::About) -----------------------
  // Quiet and plain. The IP is the big thing (36): it is the one value someone
  // stands at the machine to copy. Version is static text set once in init().
  lv_obj_t* aboutPanel;
  lv_obj_t* aboutIpValue;
  lv_obj_t* aboutUptimeValue;

  // OTA screen (separate screen, unchanged by the redesign)
  lv_obj_t* updateSlider;
  lv_obj_t* updateLabel;

  // --- Redraw suppression ---------------------------------------------------
  // Display::update() re-runs every draw*() at 10 Hz, and LVGL's setters for
  // text / position / style invalidate unconditionally (unlike lv_bar_set_value,
  // which does compare) -- so pushing an unchanged value still repaints the
  // area every tick. These caches hold the last value actually pushed so an
  // unchanged one costs a comparison instead of a repaint.
  enum TextSlot {
    TS_MODE, TS_UNIT, TS_RPM, TS_PITCH, TS_PITCH_UNIT,
    TS_TRAVEL_POS, TS_TRAVEL_UNIT, TS_TRAVEL_LEFT, TS_TRAVEL_RIGHT,
    TS_STATE,
    // Overlay slots. The title and the two hints are NOT here: every string
    // they can hold is a compile-time literal, so drawOverlay() caches the
    // VARIANT (m_lastFocus / m_lastHintVariant) and pushes the literal only
    // when that changes. Cheaper than a strcmp, and it sidesteps the
    // TEXT_SLOT_LEN cap -- a cached string longer than 19 bytes would be
    // truncated by setLabelText() and then never compare equal again, which
    // silently turns the cache into a per-tick repaint.
    TS_OV_TICK_PREV, TS_OV_TICK_VALUE, TS_OV_TICK_NEXT, TS_OV_TICK_UNIT,
    TS_OV_STOP_LEFT, TS_OV_STOP_RIGHT, TS_OV_STOP_POS,
    // Menu carousel. Safe to cache: the longest tile name is "Software update"
    // at 15 bytes and the position reads "9 / 9", both well inside
    // TEXT_SLOT_LEN - 1. Any tile name of 20 bytes or more would be truncated
    // into the cache, never compare equal again, and silently repaint at 10 Hz
    // forever; menuTileName() says so at its definition.
    TS_OV_MENU_POS, TS_OV_MENU_NAME, TS_OV_MENU_PREV, TS_OV_MENU_NEXT,
    // Diagnostics. Every string is short and bounded by its formatter (the
    // error clamps to +-999.99 mm / +-99999 p before formatting, the rates are
    // %.2f / %d), so nothing here can approach the TEXT_SLOT_LEN cap.
    TS_DIAG_ERR, TS_DIAG_ERR_P, TS_DIAG_RPM, TS_DIAG_CARRIAGE, TS_DIAG_EXPECT,
    TS_DIAG_SYNC, TS_DIAG_ANCHOR,
    // The title / capture-status row. The longest string formatCaptureStatus()
    // can produce is 18 bytes ("FULL: STOP TO SEND"), asserted against this
    // cap by test/test_debugcapture so it cannot silently grow past it.
    TS_DIAG_TITLE,
    // About. Longest possible IP is "255.255.255.255" -- 15 bytes, inside the
    // cap; the uptime formats are at most 8 bytes ("999d 23h").
    TS_ABOUT_IP, TS_ABOUT_UPTIME,
    TS_COUNT
  };
  // Plain enum constant, not a `static const size_t`: an in-class static const
  // still needs an out-of-line definition if it is ever ODR-used (passed by
  // value to snprintf, say) before C++17, and this file must build under
  // whatever standard the ESP32 core picks.
  enum { TEXT_SLOT_LEN = 20 };
  char m_textCache[TS_COUNT][TEXT_SLOT_LEN];

  const void* m_lastFeedSrc;   // last lv_image_set_src() source for feedSymbolObj
  int m_lastSyncState;         // last GlobalThreadSyncState pushed as a colour
  int m_lastMotionMode;        // last GlobalMotionMode pushed as a colour
  int m_lastDatumSource;       // last DroDatumSource pushed as label emphasis
  bool m_lastRpmNegative;
  int m_lastCarriageX;         // last x of travelCarriage (-1 = not placed)
  bool m_lastCarriageShown;
  bool m_lastLeftStopSet;
  bool m_lastRightStopSet;

  // Overlay caches. Separate from the band-4 ones above because they drive
  // separate objects (the panel keeps its own markers/carriage), and because
  // the overlay is not drawn at all while it is hidden -- so these must not be
  // disturbed by the main screen's per-tick updates.
  int m_lastFocus;                // last UiFocus rendered (-1 = nothing yet)
  int m_lastHintVariant;          // last OverlayHint pushed into the hint row
  int m_lastModeTile;             // last mode tile filled with the accent
  bool m_lastOverlayLeftStopSet;
  bool m_lastOverlayRightStopSet;
  int m_lastOverlayCarriageX;
  bool m_lastOverlayCarriageShown;
  // The clear-both confirm bar: whether its three objects are shown, and the
  // last fill width pushed. The display polls at 10 Hz against a 1 s hold, so
  // the fill advances in ~10 visible steps -- that is the design, not jank
  // (there is no animation on this build). Width, not permille: two permilles
  // that quantise to the same pixel width must not repaint.
  bool m_lastStopsConfirmShown;
  int m_lastStopsConfirmFillW;
  // Last MenuTileBlock pushed as the ink/fill of each of the three visible
  // tiles. Only the block state drives colour, so arrowing between two tiles
  // that are both live costs no restyle at all.
  int m_lastMenuBlock;
  int m_lastMenuPrevBlock;
  int m_lastMenuNextBlock;
  // Whether each neighbour tile is currently shown: at the two ends of the
  // (non-wrapping) ring there is no previous / next tile, and an empty bordered
  // box would read as a broken widget, so the whole tile is hidden instead.
  // Reset to TRUE (not false) -- init() builds the tiles visible, and the cache
  // must describe the tree it gates.
  bool m_lastMenuPrevShown;
  bool m_lastMenuNextShown;
  // Pitch pip row (band 3). Count gates the reflow (positions + surplus
  // hiding), index gates the restyle; a count change forces the restyle too by
  // resetting the index cache, because the reflow re-homes every pip.
  int m_lastPipCount;
  int m_lastPipIndex;
  // DRO datum picker: the selected tile last styled (-1 = not styled yet, so
  // the first draw after a rebuild always paints the selection).
  int m_lastDatumTile;
  // Diagnostics caches. Marker x is a position (like m_lastCarriageX); pegged
  // is the off-scale state that turns the marker colourFault; sync mirrors
  // m_lastSyncState for the diag chip.
  int m_lastDiagErrX;
  bool m_lastDiagErrPegged;
  int m_lastDiagSyncState;
  // About: -1 = never drawn, else 0/1 connected state driving the IP colour.
  int m_lastAboutConnected;

  // About-screen network state on HOST builds only. The device reads
  // WiFi.localIP()/WiFi.status() directly in drawAbout() (guarded include in
  // the .cpp); the host has no WiFi, so the test/screenshot harness injects
  // the address instead. Members exist on both builds (CLAUDE.md: constructors
  // must initialise all members) but the device path never reads them.
  IPAddress m_aboutIp;
  bool m_aboutConnected;

  void initDisplay();
  void initialiseOta();
  void resetObjectTree();   // null every object pointer + clear the caches
  // Pushes text only when it differs from the cached value for `slot`; returns
  // true when the label was actually updated.
  bool setLabelText(lv_obj_t* label, int slot, const char* text);

public:
  // Definitions in .cpp: both need PALETTE_DARK/PALETTE_LIGHT (static, defined
  // there) to pick the initial m_palette, so they can no longer be inline here.
  // `ui` may be nullptr (and always is on the Wi-Fi-setup path); the overlay
  // then never opens.
  Display(Spindle* spindle, Leadscrew* leadscrew, const UiState* ui);
  Display();

  void init();
  void update();
  // Runtime theme switch -- see .cpp for how it rebuilds the screen. Driven by
  // the "Theme" menu tile (ButtonPad::activateMenuTile()), which persists the
  // new value FIRST and only calls this once the write actually succeeded.
  void setTheme(uint8_t theme);
  // Runtime DRO datum switch, driven by the "DRO datum" menu tile on the same
  // save-then-apply order as setTheme(). See m_droDatum above for why the
  // display has to hold this rather than re-reading LatheConfig.
  void setDroDatum(DroDatumPreference datum);
  void showWifi(const char * ssid, const char * password, IPAddress ip);
  void showConnected(IPAddress ip);
#if PIO_UNIT_TESTING
  // Host harness only (tests / the screenshot renderer): inject what the About
  // screen should report, since there is no WiFi to ask. Compiled out on the
  // device, where drawAbout() asks WiFi directly.
  void hostSetAboutNetwork(IPAddress ip, bool connected) {
    m_aboutIp = ip;
    m_aboutConnected = connected;
  }
#endif

protected:
  void initvars();
  void drawStatusBar();   // band 1
  void drawSpindleRpm();  // band 1 (right)
  void drawMode();        // band 2 glyph
  void drawPitch();       // band 2 value + band 3 ticker
  void drawTravel();      // band 4
  void drawStateBar();    // band 5
  void drawOverlay();     // the selector overlay, on top of bands 2-4
  // Called by drawOverlay() only, once the panel is known to be visible.
  void drawOverlayTicker(bool jogSpeed);
  void drawOverlayMode();
  // `confirmPermille` is UiState::stopsConfirmPermille() -- 0..1000, 0 when the
  // clear-both hold is not running -- computed once in drawOverlay() so the
  // hint row and the confirm bar are driven by the SAME sample and cannot
  // disagree about whether the hold is live.
  void drawOverlayStops(int confirmPermille);
  // The DRO datum picker (group E). Renders m_droDatum as the selection.
  void drawOverlayDatum();
  // The two full read-only screens. Routed through drawOverlay()'s focus
  // switch like the widgets, but they live on their own full-screen panels
  // (diagPanel / aboutPanel), not on overlayPanel.
  void drawDiagnostics();
  void drawAbout();
  // Takes the two machine facts rather than a per-tile verdict: the carousel
  // shows THREE tiles and has to state the availability of each, and running
  // them all through the one menuTileBlock() is what keeps the card, the two
  // neighbours and drawOverlay()'s hint from ever disagreeing.
  void drawOverlayMenu(bool motionActive, bool threadMode);
  void updateLed();
  void writeLed();
  void drawOTA();
};
