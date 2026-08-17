
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
// The eight 32x32 status icons below are NOT drawn by the redesigned main
// screen (docs/ux-redesign.md section 8): the stop/sync/enable/lock chips they
// used to fill are gone -- their information now lives in the status bar, the
// carriage-travel band and the state bar as text and colour. The declarations
// are kept because the assets are still compiled in and the overlays/menu
// (FS-I3) are the obvious next consumer; if that turns out not to be the case
// they, and lib/display/icons/*.c, can be deleted together.
LV_IMAGE_DECLARE(leftstop);
LV_IMAGE_DECLARE(rightstop);
LV_IMAGE_DECLARE(unlocked);   // lock is removed from the design (section 7) --
LV_IMAGE_DECLARE(locked);     // these two have no remaining consumer at all.
LV_IMAGE_DECLARE(left);
LV_IMAGE_DECLARE(right);
LV_IMAGE_DECLARE(pauseSymbol);
LV_IMAGE_DECLARE(syncSymbol);
LV_IMAGE_DECLARE(jog);

#define DRAW_BUF_SIZE ((TFT_WIDTH * TFT_HEIGHT / 10) * (LV_COLOR_DEPTH / 8))

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

  // Band 3, pitch ticker (y 125..145)
  lv_obj_t* pitchSlider;

  // Band 4, carriage travel -- the small DRO (y 147..191)
  lv_obj_t* travelTrack;      // the full-width track
  lv_obj_t* travelLeftMark;   // left stop marker (hidden when unset)
  lv_obj_t* travelRightMark;  // right stop marker (hidden when unset)
  lv_obj_t* travelCarriage;   // live carriage position on the track
  lv_obj_t* travelLeftLabel;  // "L <pos>" / "L --"
  lv_obj_t* travelRightLabel; // "<pos> R" / "-- R"
  lv_obj_t* travelPosLabel;   // live carriage position, datum-relative
  lv_obj_t* travelPosUnit;    // mm / in

  // Band 5, state + soft keys (y 193..239)
  lv_obj_t* stateDot;
  lv_obj_t* stateLabel;
  lv_obj_t* softKeyLabel[3];  // HALT / MENU / RUN|STOP

  lv_obj_t* bandRule[4];      // the four band separators

  // --- Selector overlay (docs/ux-redesign.md section 4) ----------------------
  // ONE panel, built once in init() alongside the main screen and left hidden.
  // Its three content groups are swapped by drawOverlay() as focus moves; the
  // panel is never rebuilt or deleted (a rebuild at 10 Hz would be wasteful and
  // would defeat the redraw caches below). It is created LAST so it sits on top
  // of the main screen in the sibling z-order, and it is opaque, so whatever it
  // covers simply is not visible -- LV_DRAW_LAYER_SIMPLE_BUF_SIZE is 24 KB and a
  // translucent panel this size needs ~84 KB (section 8).
  //
  // UiFocus::Menu deliberately renders NOTHING here (the main screen stays
  // visible): the menu carousel and its tile actions are owned by the menu
  // feature set, not this one.
  lv_obj_t* overlayPanel;      // the solid ground + accent border
  lv_obj_t* overlayTitle;      // PITCH / JOG SPEED / MODE / STOPS
  lv_obj_t* overlayHintLeft;   // what the arrows do
  lv_obj_t* overlayHintRight;  // "OK done"

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
    TS_STATE, TS_SOFTKEY,
    // Overlay slots. The title and the two hints are NOT here: every string
    // they can hold is a compile-time literal, so drawOverlay() caches the
    // VARIANT (m_lastFocus / m_lastHintVariant) and pushes the literal only
    // when that changes. Cheaper than a strcmp, and it sidesteps the
    // TEXT_SLOT_LEN cap -- a cached string longer than 19 bytes would be
    // truncated by setLabelText() and then never compare equal again, which
    // silently turns the cache into a per-tick repaint.
    TS_OV_TICK_PREV, TS_OV_TICK_VALUE, TS_OV_TICK_NEXT, TS_OV_TICK_UNIT,
    TS_OV_STOP_LEFT, TS_OV_STOP_RIGHT, TS_OV_STOP_POS,
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
  // Runtime theme switch -- see .cpp for how it rebuilds the screen. Not
  // wired to any UI yet (no menu exists), but reachable for when one does.
  void setTheme(uint8_t theme);
  void showWifi(const char * ssid, const char * password, IPAddress ip);
  void showConnected(IPAddress ip);

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
  void drawOverlayStops();
  void updateLed();
  void writeLed();
  void drawOTA();
};
