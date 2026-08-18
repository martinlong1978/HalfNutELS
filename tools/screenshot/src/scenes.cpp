#include "scenes.h"

// C++ library headers BEFORE <Arduino.h>: the host Arduino stub defines
// min/max as macros, which breaks any libstdc++ header parsed after them (see
// the note at the top of shim/Arduino.h).
#include <cstring>
#include <cstdio>
#include <string>

#include <Arduino.h>

#include <config.h>
#include <globalstate.h>
#include <latheconfig.h>
#include <leadscrew.h>
#include <spindle.h>
#include <uistate.h>

#include <lvgl.h>

#include "leadscrewio_mock.h"      // test/test_leadscrew -- the unit tests' own double
#include "ST7789_320_240displaylvgl.h"

#include "framebuffer.h"

namespace shot {
namespace {

// --- The rig -----------------------------------------------------------------
// Exactly what src/main.cpp builds, minus the hardware: the same construction
// order (config -> derived -> spindle -> leadscrew -> UiState -> Display) and
// the same arguments, so the Display sees objects wired the way the firmware
// wires them.
struct Rig {
  LatheConfig cfg;
  LatheConfigDerived* derived = nullptr;
  Spindle* spindle = nullptr;
  LeadscrewIOMock io;
  Leadscrew* ls = nullptr;
  GlobalState* gs = nullptr;
  UiState ui;
  Display* display = nullptr;

  void build(uint8_t theme) {
    cfg = LatheConfig();
    cfg.check = CHECKVALUE;
    cfg.theme = theme;
    derived = new LatheConfigDerived(&cfg);
    gs = GlobalState::getInstance();
    spindle = new Spindle(ELS_SPINDLE_ENCODER_A, ELS_SPINDLE_ENCODER_B, derived);
    ls = new Leadscrew(derived, spindle, &io,
                       derived->accellerationPulseSec(),
                       derived->leadscrewInitialPulseDelay(),
                       (int)(derived->stepperPpr() * derived->gearboxRatio()),
                       derived->leadscrewPitchMm(), derived->spindleEncoderPpr());
    display = new Display(spindle, ls, &ui);
  }
};

// --- Driving the inputs ------------------------------------------------------

// Turn the spindle at a steady rate for `ms` of virtual time, the same way
// test/test_thread_sync does: fixed 100 us steps carrying the fractional pulse
// remainder. Needed because getEstimatedVelocityInRPM() is a WINDOWED measure
// (m_lastRev*) -- setting a position gives no velocity at all, so an
// un-spun rig renders "0 RPM" and the status bar is never exercised.
//
// NOTE the sign. Both Spindle implementations return
// -(revSize * 60e6) / (revMicros * ppr), so a NEGATIVE pulse delta is what the
// screen shows as a positive RPM. Passing a positive rpm here therefore feeds
// negative pulses; pass a negative rpm to exercise the reverse-spindle
// (colourFault) branch of drawSpindleRpm().
void spin(Rig& r, float rpm, int ms) {
  const int ppr = r.derived->spindleEncoderPpr();
  const float pps = -(rpm / 60.0f) * (float)ppr;
  const uint64_t dt = 100;  // us per step
  const int steps = (int)((uint64_t)ms * 1000ULL / dt);
  float carry = 0.0f;
  for (int i = 0; i < steps; i++) {
    advanceMockMicros(dt);
    carry += pps * ((float)dt / 1000000.0f);
    const int whole = (int)carry;
    if (whole != 0) {
      carry -= (float)whole;
      r.spindle->incrementCurrentPosition(whole);
    }
  }
}

void setFeedMode(Rig& r, GlobalFeedMode want) {
  for (int guard = 0; guard < 8 && r.gs->getFeedMode() != want; guard++) {
    r.gs->IncFeedMode();
  }
}

// spin() plus the SpindleTask's other half: run Leadscrew::update() every
// virtual-clock step, exactly as test/test_thread_sync drives a cut. This is
// what makes the Diagnostics screen's numbers REAL -- expected position,
// position error, pulse output and the leadscrew's own velocity estimate all
// only move inside update(). Push the current pitch first, the way ButtonPad
// does after a rate/mode change (the Leadscrew only reads it at construction).
void spinDriven(Rig& r, float rpm, int ms) {
  r.ls->setTargetPitchMM(r.gs->getCurrentFeedPitch());
  const int ppr = r.derived->spindleEncoderPpr();
  const float pps = -(rpm / 60.0f) * (float)ppr;
  // 10 us, not spin()'s 100: the leadscrew's velocity estimate is the
  // reciprocal of ONE inter-pulse gap, so the clock step quantises it -- at
  // 100 us a ~600 us true period reads up to ~20% fast, which put a fake
  // mismatch in the Diagnostics CARRIAGE column. 10 us keeps it under ~2%.
  const uint64_t dt = 10;  // us per step
  const int steps = (int)((uint64_t)ms * 1000ULL / dt);
  float carry = 0.0f;
  for (int i = 0; i < steps; i++) {
    advanceMockMicros(dt);
    carry += pps * ((float)dt / 1000000.0f);
    const int whole = (int)carry;
    if (whole != 0) {
      carry -= (float)whole;
      r.spindle->incrementCurrentPosition(whole);
    }
    r.ls->update();
  }
}

int pulsesForMM(Rig& r, float mm) {
  return (int)(mm * r.derived->leadscrewStepsPerMm());
}

// Focus is private to UiState by design (it is a decision function, not a bag
// of setters), so scenes reach it the only legitimate way: by pressing the keys
// an operator would press. This is a feature, not a workaround -- a focus the
// keypad cannot reach is a focus the screenshot should not claim exists.
UiContext ctxOf(Rig& r) {
  UiContext c;
  c.leftStopSet = r.ls->getStopPositionState(LeadscrewStopPosition::LEFT) ==
                  LeadscrewStopState::SET;
  c.rightStopSet = r.ls->getStopPositionState(LeadscrewStopPosition::RIGHT) ==
                   LeadscrewStopState::SET;
  const GlobalMotionMode m = r.gs->getMotionMode();
  c.motionEnabled = (m == MM_ENABLED);
  c.motionActive = (m != MM_DISABLED && m != MM_UNSET);
  return c;
}

void key(Rig& r, UiKey k, UiKeyEvent ev) {
  r.ui.handleKey(k, ev, ctxOf(r), millis());
}

// --- Common starting point ---------------------------------------------------
// Metric feed, both stops set 48 mm apart, carriage 12.40 mm along, spindle
// turning. Scenes then change only what they are about.
void baseState(Rig& r) {
  r.gs->setUnitMode(METRIC);
  setFeedMode(r, FM_FEED);
  r.gs->setMotionMode(MM_DISABLED);
  r.ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
  r.ls->setStopPosition(LeadscrewStopPosition::RIGHT, pulsesForMM(r, 48.0f));
  r.ls->setCurrentPosition(pulsesForMM(r, 12.4f));
  // AFTER the stops, not before: Leadscrew::setStopPosition() raises SS_SYNC
  // when it latches the helix anchor (leadscrew.cpp), so setting it first would
  // be silently overwritten and every scene would show a green SYNC.
  r.gs->setThreadSyncState(SS_UNSYNC);
}

// --- The refresh pump --------------------------------------------------------
// Display::update() runs lv_timer_handler() BEFORE the draw*() calls, so a value
// pushed on tick N is not rendered until tick N+1. And LVGL's refresh timer only
// runs once LV_DEF_REFR_PERIOD (33 ms) of tick time has passed, which is why the
// virtual clock has to move between calls. Several ticks, not two, because the
// overlay's show/hide and the label re-layouts each invalidate on the tick after
// the one that changed them.
void pump(Display* d, int ticks = 6) {
  for (int i = 0; i < ticks; i++) {
    advanceMockMicros(40000);  // 40 ms > LV_DEF_REFR_PERIOD
    d->update();
  }
}

// ---------------------------------------------------------------------------
// The scenes.
// ---------------------------------------------------------------------------

struct SceneDef {
  const char* name;
  void (*apply)(Rig&);
  uint8_t theme;
  bool wifiScreen;  // uses the parameterless Display + showWifi/showConnected
  int wifiVariant;  // 0 = join screen, 1 = connected screen
};

void sc_restMetricFeed(Rig& r) { baseState(r); spin(r, 850, 200); }

void sc_restMetricThreadR(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setThreadSyncState(SS_SYNC);
  spin(r, 320, 300);
}

void sc_restMetricThreadL(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD_REVERSE);
  spin(r, 320, 300);
}

void sc_restImperialThread(Rig& r) {
  baseState(r);
  r.gs->setUnitMode(IMPERIAL);
  setFeedMode(r, FM_THREAD);
  r.gs->setThreadSyncState(SS_SYNC);
  spin(r, 420, 300);
}

void sc_restImperialFeed(Rig& r) {
  baseState(r);
  r.gs->setUnitMode(IMPERIAL);
  setFeedMode(r, FM_FEED);
  spin(r, 900, 200);
}

// The reverse-spindle branch of drawSpindleRpm(): the value goes colourFault.
// Not on the brief, but it is the one branch of this file that has been wrong
// twice (see the comment at drawSpindleRpm), so it gets an image.
void sc_restReverseSpindle(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spin(r, -300, 300);
}

void sc_stateCutting(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setThreadSyncState(SS_SYNC);
  r.gs->setMotionMode(MM_ENABLED);
  spin(r, 320, 300);
}

void sc_stateJogging(Rig& r) {
  baseState(r);
  r.gs->setMotionMode(MM_INTERACTIVE_JOG_RIGHT);
  spin(r, 0, 10);
}

void sc_stateReturning(Rig& r) {
  baseState(r);
  r.gs->setMotionMode(MM_JOG_LEFT);
  spin(r, 0, 10);
}

void sc_stateDecelerating(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setMotionMode(MM_DECELLERATE);
  spin(r, 180, 300);
}

void sc_stopsBoth(Rig& r) { baseState(r); spin(r, 850, 200); }

void sc_stopsLeftOnly(Rig& r) {
  baseState(r);
  r.ls->unsetStopPosition(LeadscrewStopPosition::RIGHT);
  spin(r, 850, 200);
}

void sc_stopsRightOnly(Rig& r) {
  baseState(r);
  r.ls->unsetStopPosition(LeadscrewStopPosition::LEFT);
  spin(r, 850, 200);
}

void sc_stopsNone(Rig& r) {
  baseState(r);
  r.ls->unsetStopPosition(LeadscrewStopPosition::LEFT);
  r.ls->unsetStopPosition(LeadscrewStopPosition::RIGHT);
  spin(r, 850, 200);
}

void sc_overlayMode(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spin(r, 320, 300);
  key(r, UiKey::Mode, UiKeyEvent::Click);
}

void sc_overlayRate(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spin(r, 320, 300);
  key(r, UiKey::Rate, UiKeyEvent::Click);
}

void sc_overlayJogSpeed(Rig& r) {
  baseState(r);
  spin(r, 850, 200);
  key(r, UiKey::Ok, UiKeyEvent::Click);  // OK at rest opens JOG SPEED
}

void sc_overlayStops(Rig& r) {
  baseState(r);
  spin(r, 850, 200);
  // The FULL short-press event train (KeyArray's vocabulary: Press, Click,
  // Release), not a bare Press: the Press now ARMS the clear-both confirm bar
  // (both stops are set and the carriage is at rest), and pump() advances
  // 240 ms of virtual time -- a key left "down" would render a quarter-full
  // confirm bar instead of the resting widget this scene exists to pin.
  key(r, UiKey::Stops, UiKeyEvent::Press);
  key(r, UiKey::Stops, UiKeyEvent::Click);
  key(r, UiKey::Stops, UiKeyEvent::Release);
}

// The clear-both confirm bar (docs/ux-redesign.md section 4: "STOPS hold -
// clear both, after a 1 s confirm bar") at three fill levels, so the gallery
// shows the growth and the label together. Driven exactly as the keypad would:
// STOPS goes down (Press arms the gesture -- both stops set, carriage at
// rest), the key STAYS down, and the virtual clock advances. The bar is drawn
// live from stopsConfirmPermille(millis()) at every one of pump()'s six ticks.
// pump() advances 6 x 40 ms, and a value pushed on tick N is rendered on tick
// N+1 (see pump()'s own comment), so the width in the PNG is the one pushed on
// tick 5: the image lands at (holdMs + 200) / 1000 of the second. Verified
// against the rendered fill's pixel extent, not assumed.
void holdStops(Rig& r, int holdMs) {
  baseState(r);
  spin(r, 850, 200);
  key(r, UiKey::Stops, UiKeyEvent::Press);
  advanceMockMicros((uint64_t)holdMs * 1000ULL);
}

void sc_stopsConfirm25(Rig& r) { holdStops(r, 50); }   // renders at 250/1000
void sc_stopsConfirm60(Rig& r) { holdStops(r, 400); }  // renders at 600/1000
void sc_stopsConfirm95(Rig& r) { holdStops(r, 750); }  // renders at 950/1000

// STOPS while the carriage is under power: the hint row must say the edit is
// refused rather than advertise a gesture UiState will ignore.
void sc_overlayStopsLocked(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spin(r, 320, 300);
  r.gs->setMotionMode(MM_ENABLED);
  key(r, UiKey::Stops, UiKeyEvent::Press);
}

void openMenuAt(Rig& r, int index) {
  key(r, UiKey::Menu, UiKeyEvent::Click);  // opens at 0
  for (int i = 0; i < index; i++) {
    key(r, UiKey::Right, UiKeyEvent::Click);
  }
}

void sc_menuUnits(Rig& r) {  // tile 0, live
  baseState(r);
  spin(r, 850, 200);
  openMenuAt(r, MENU_UNITS);
}

void sc_menuDroDatum(Rig& r) {  // tile 2, live at rest, blocked neighbour-free
  baseState(r);
  spin(r, 850, 200);
  openMenuAt(r, MENU_DRO_DATUM);
}

// Tile 4 (Sync) in FEED mode: MTB_FEED_MODE. Card greys, hint reads
// "needs thread mode", and the right-hand "OK open" disappears.
void sc_menuSyncBlocked(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_FEED);
  spin(r, 850, 200);
  openMenuAt(r, MENU_SYNC);
}

// Tile 5 (Software update) with the leadscrew engaged: MTB_MOTION. Both
// neighbours (Sync, Wi-Fi setup) are blocked too, so this is also the image
// that shows the colourCaution neighbour treatment.
void sc_menuUpdateBlocked(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setMotionMode(MM_ENABLED);
  spin(r, 320, 300);
  openMenuAt(r, MENU_SOFTWARE_UPDATE);
}

void sc_menuAbout(Rig& r) {  // the far end of the ring: blank right neighbour
  baseState(r);
  spin(r, 850, 200);
  openMenuAt(r, MENU_ABOUT);
}

// --- The three menu destinations (UiFocus::DroDatum / Diagnostics / About) --
// All reached the only legitimate way: menu open, arrows to the tile, OK --
// which exercises menuTileDestination() and the carousel-close in the same
// image. See ctxOf(): the tile is live because the carriage is at rest.

void sc_overlayDatum(Rig& r) {
  baseState(r);
  spin(r, 850, 200);
  openMenuAt(r, MENU_DRO_DATUM);
  key(r, UiKey::Ok, UiKeyEvent::Click);  // activate -> UiFocus::DroDatum
}

// Motion starting UNDER an already-open datum picker (a web-UI engage, a run
// finishing late): the arrows go dead in UiState, and the hint row must swap
// to the amber "moving - datum locked" chip rather than keep offering them.
// The picker itself was opened at rest -- menuTileBlock() would have refused
// the tile under power, which is why the motion is set afterwards.
void sc_overlayDatumLocked(Rig& r) {
  baseState(r);
  spin(r, 320, 200);
  openMenuAt(r, MENU_DRO_DATUM);
  key(r, UiKey::Ok, UiKeyEvent::Click);
  r.gs->setMotionMode(MM_ENABLED);
}

// Diagnostics mid-cut, with the leadscrew genuinely driven (spinDriven) so
// every number on the screen is the machine's own: a live sub-pip position
// error, a real carriage velocity beside its expectation, SYNCED chip.
void sc_diagnostics(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spinDriven(r, 0, 5);  // settle expected==current while still disabled
                        // (this also publishes SS_UNSYNC, so SYNC comes after)
  r.gs->setThreadSyncState(SS_SYNC);
  r.gs->setMotionMode(MM_ENABLED);
  // Long enough for the acceleration planner's catch-up margin to settle, so
  // CARRIAGE is read at ratio speed rather than mid-overshoot.
  spinDriven(r, 320, 900);
  openMenuAt(r, MENU_DIAGNOSTICS);
  key(r, UiKey::Ok, UiKeyEvent::Click);
}

// Diagnostics with UGLY values: the axis is commanded but update() never runs
// (a wedged SpindleTask, the fault this screen exists to catch), so the
// expected position is stranded 12.4 mm from the carriage. The error bar must
// peg at full deflection in colourFault, the 48 must show a large negative
// number, and CARRIAGE 0.00 must sit beside a non-zero EXPECT.
void sc_diagnosticsError(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setThreadSyncState(SS_UNSYNC);
  r.gs->setMotionMode(MM_ENABLED);
  spin(r, 850, 300);  // spindle turns; the leadscrew never updates
  openMenuAt(r, MENU_DIAGNOSTICS);
  key(r, UiKey::Ok, UiKeyEvent::Click);
}

// Diagnostics with a MANUAL sync anchor: the operator pressed sync with the
// tool on the thread rather than letting a stop latch it. baseState()'s
// setStopPosition(LEFT, 0) latches a LEFT anchor first (stop == carriage at
// that moment), so this scene exercises exactly the "last call wins" override
// setSyncPoint() documents -- and the bottom row must read "manual", not
// "L stop", which is what the ANCHOR readout exists to distinguish.
void sc_diagnosticsManualAnchor(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  spinDriven(r, 0, 5);   // settle expected==current while still disabled
  r.ls->setSyncPoint();  // MANUAL anchor; raises SS_SYNC itself
  r.gs->setMotionMode(MM_ENABLED);
  spinDriven(r, 320, 900);
  openMenuAt(r, MENU_DIAGNOSTICS);
  key(r, UiKey::Ok, UiKeyEvent::Click);
}

void sc_about(Rig& r) {
  baseState(r);
  spin(r, 850, 200);
  // 3 h 24 m of virtual uptime, so the formatter's h/m branch is on screen.
  advanceMockMicros((uint64_t)(3 * 3600 + 24 * 60) * 1000000ULL);
  r.display->hostSetAboutNetwork(IPAddress(192, 168, 1, 123), true);
  openMenuAt(r, MENU_ABOUT);
  key(r, UiKey::Ok, UiKeyEvent::Click);
}

void sc_otaDownloading(Rig& r) {
  baseState(r);
  r.gs->setOTA();
  r.gs->setOtaStatus(OTA_DOWNLOADING);
  r.gs->setOTAContentLength(1400000);
  r.gs->setOTABytes(861000);
}

void sc_otaChecking(Rig& r) {
  baseState(r);
  r.gs->setOTA();
  r.gs->setOtaStatus(OTA_CHECKING);
}

void sc_otaNoUpdate(Rig& r) {
  baseState(r);
  r.gs->setOTA();
  r.gs->setOtaStatus(OTA_NO_UPDATE);
}

const SceneDef kScenes[] = {
  // Rest screens, one per (mode x unit) the readout formats differently.
  { "rest-metric-feed",       sc_restMetricFeed,      THEME_DARK,  false, 0 },
  { "rest-metric-thread-r",   sc_restMetricThreadR,   THEME_DARK,  false, 0 },
  { "rest-metric-thread-l",   sc_restMetricThreadL,   THEME_DARK,  false, 0 },
  { "rest-imperial-thread",   sc_restImperialThread,  THEME_DARK,  false, 0 },
  { "rest-imperial-feed",     sc_restImperialFeed,    THEME_DARK,  false, 0 },
  { "rest-reverse-spindle",   sc_restReverseSpindle,  THEME_DARK,  false, 0 },
  // Machine states (band 5).
  { "state-cutting",          sc_stateCutting,        THEME_DARK,  false, 0 },
  { "state-jogging",          sc_stateJogging,        THEME_DARK,  false, 0 },
  { "state-returning",        sc_stateReturning,      THEME_DARK,  false, 0 },
  { "state-decelerating",     sc_stateDecelerating,   THEME_DARK,  false, 0 },
  // Endstop combinations (band 4).
  { "stops-both",             sc_stopsBoth,           THEME_DARK,  false, 0 },
  { "stops-left-only",        sc_stopsLeftOnly,       THEME_DARK,  false, 0 },
  { "stops-right-only",       sc_stopsRightOnly,      THEME_DARK,  false, 0 },
  { "stops-none",             sc_stopsNone,           THEME_DARK,  false, 0 },
  // Selector overlays.
  { "overlay-mode",           sc_overlayMode,         THEME_DARK,  false, 0 },
  { "overlay-rate",           sc_overlayRate,         THEME_DARK,  false, 0 },
  { "overlay-jogspeed",       sc_overlayJogSpeed,     THEME_DARK,  false, 0 },
  { "overlay-stops",          sc_overlayStops,        THEME_DARK,  false, 0 },
  { "overlay-stops-locked",   sc_overlayStopsLocked,  THEME_DARK,  false, 0 },
  // The clear-both confirm bar mid-hold, three fill levels.
  { "overlay-stops-confirm-25", sc_stopsConfirm25,    THEME_DARK,  false, 0 },
  { "overlay-stops-confirm-60", sc_stopsConfirm60,    THEME_DARK,  false, 0 },
  { "overlay-stops-confirm-95", sc_stopsConfirm95,    THEME_DARK,  false, 0 },
  // Menu carousel.
  { "menu-units",             sc_menuUnits,           THEME_DARK,  false, 0 },
  { "menu-dro-datum",         sc_menuDroDatum,        THEME_DARK,  false, 0 },
  { "menu-sync-blocked",      sc_menuSyncBlocked,     THEME_DARK,  false, 0 },
  { "menu-update-blocked",    sc_menuUpdateBlocked,   THEME_DARK,  false, 0 },
  { "menu-about",             sc_menuAbout,           THEME_DARK,  false, 0 },
  // The three menu destinations.
  { "overlay-datum",          sc_overlayDatum,        THEME_DARK,  false, 0 },
  { "overlay-datum-locked",   sc_overlayDatumLocked,  THEME_DARK,  false, 0 },
  { "diagnostics",            sc_diagnostics,         THEME_DARK,  false, 0 },
  { "diagnostics-error",      sc_diagnosticsError,    THEME_DARK,  false, 0 },
  { "diagnostics-manual",     sc_diagnosticsManualAnchor, THEME_DARK, false, 0 },
  { "about",                  sc_about,               THEME_DARK,  false, 0 },
  // Light palette. Same two states as their dark counterparts above, so the
  // pair is directly comparable.
  { "light-rest-metric-feed", sc_restMetricFeed,      THEME_LIGHT, false, 0 },
  { "light-overlay-mode",     sc_overlayMode,         THEME_LIGHT, false, 0 },
  { "light-menu-sync-blocked",sc_menuSyncBlocked,     THEME_LIGHT, false, 0 },
  // OTA screen (a separate screen, not the dashboard).
  { "ota-downloading",        sc_otaDownloading,      THEME_DARK,  false, 0 },
  { "ota-checking",           sc_otaChecking,         THEME_DARK,  false, 0 },
  { "ota-no-update",          sc_otaNoUpdate,         THEME_DARK,  false, 0 },
  // Wi-Fi setup path: the parameterless Display, no dashboard at all.
  { "wifi-setup",             nullptr,                THEME_DARK,  true,  0 },
  { "wifi-connected",         nullptr,                THEME_DARK,  true,  1 },
};

const char* g_names[sizeof(kScenes) / sizeof(kScenes[0])];

}  // namespace

const char* const* sceneNames() {
  for (size_t i = 0; i < sceneCount(); i++) {
    g_names[i] = kScenes[i].name;
  }
  return g_names;
}

size_t sceneCount() { return sizeof(kScenes) / sizeof(kScenes[0]); }

bool renderScene(const char* name) {
  const SceneDef* def = nullptr;
  for (size_t i = 0; i < sceneCount(); i++) {
    if (strcmp(kScenes[i].name, name) == 0) {
      def = &kScenes[i];
      break;
    }
  }
  if (def == nullptr) {
    return false;
  }

  resetMockClock();
  // Start the clock off zero: LVGL treats tick 0 as "never run" for some timer
  // bookkeeping, and Display's own caches are keyed on values, not time.
  setMockMicros(1000000);

  if (def->wifiScreen) {
    // The Wi-Fi path constructs Display with no spindle/leadscrew/UiState and
    // never calls init() -- exactly as main.cpp's runWifiSettings() does.
    Display* d = new Display();
    d->showWifi("ELS-Setup", "leadscrew", IPAddress(192, 168, 4, 1));
    if (def->wifiVariant == 1) {
      d->showConnected(IPAddress(192, 168, 4, 1));
    }
    for (int i = 0; i < 6; i++) {
      advanceMockMicros(40000);
      lv_timer_handler();
    }
    return true;
  }

  Rig r;
  r.build(def->theme);
  r.display->init();
  def->apply(r);
  pump(r.display);
  return true;
}

}  // namespace shot
