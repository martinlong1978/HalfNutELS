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
#include <otaoutcome.h>
#include <spindle.h>
#include <uistate.h>
#include <version.h>

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
  // Built the same way ButtonPad::buildContext() builds them on the device, off
  // the same GlobalState. threadMode was previously left UNSET here - an
  // uninitialised bool feeding menuTileBlock(), so the Sync tile's refusal was
  // decided by whatever was on the stack.
  const GlobalFeedMode fm = r.gs->getFeedMode();
  c.threadMode = (fm == FM_THREAD || fm == FM_THREAD_REVERSE);
  c.alarm = r.gs->alarmActive();
  // Same reasoning as threadMode above: leaving this UNSET fed garbage into
  // the new UiFocus::Ota gate the moment it existed, since UiContext is a
  // bare struct with no member-wise default. Built off the same GlobalState
  // ButtonPad::buildContext() reads.
  c.ota = r.gs->hasOTA();
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
  // The boot splash. Its own flag rather than a wifiVariant because it is the
  // other axis entirely: the splash IS themed (main.cpp shows it only on the
  // normal boot path, where a stored theme exists), so it needs the full Rig
  // like every dashboard scene -- but it must be rendered WITHOUT init(), since
  // on the device it is what stands on the screen before init() builds the
  // dashboard over it. Rows that omit this get false, which is every other one.
  bool splashScreen;
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


// --- The stepper-alarm modal ------------------------------------------------
//
// Driven exactly as the firmware drives it and no other way: the alarm task's
// GlobalState publication is set, and then UiState::tick() is given a context
// built from that same GlobalState (ctxOf). There is no key that opens this
// dialog, on the bench or on the machine, so there is none here either - which
// is also what makes these images proof that the FORCED focus works, rather
// than proof that a setter was called.
//
// Each of the four variants gets a picture, because each is a different
// sentence to the operator and the assertions in the .cpp check box arithmetic,
// not whether the wording fits or reads.
void alarmState(Rig& r, GlobalAlarmState state, bool faultPresent,
                bool clearFailed) {
  r.gs->setAlarmState(state, faultPresent, clearFailed);
  r.ui.tick(ctxOf(r), millis());
}

// The ordinary case: a fault has tripped mid-cut and is still present. Set up
// with the machine engaged and threading first, so what the modal covers is a
// screen that had live numbers on it a moment ago.
void sc_alarm(Rig& r) {
  baseState(r);
  setFeedMode(r, FM_THREAD);
  r.gs->setThreadSyncState(SS_SYNC);
  r.gs->setMotionMode(MM_ENABLED);
  spin(r, 320, 300);
  // What the alarm task does on the trip, in the order it does it.
  r.gs->setMotionMode(MM_DISABLED);
  r.gs->setThreadSyncState(SS_UNSYNC);
  alarmState(r, AS_ALARM, /*faultPresent=*/true, /*clearFailed=*/false);
}

// A momentary fault: the driver released its alarm output by itself, but the
// latch holds, because the machine still stopped and the sync still died.
void sc_alarmReleased(Rig& r) {
  baseState(r);
  alarmState(r, AS_ALARM, /*faultPresent=*/false, /*clearFailed=*/false);
}

// OK pressed, ENA pulse in flight. OK is refused for the second it lasts, so
// the chip has to stop offering it.
void sc_alarmClearing(Rig& r) {
  baseState(r);
  alarmState(r, AS_CLEARING, /*faultPresent=*/true, /*clearFailed=*/false);
}

// The reset did not take - the crash has not been freed. Without its own
// wording this is indistinguishable from OK having done nothing.
void sc_alarmFailed(Rig& r) {
  baseState(r);
  alarmState(r, AS_ALARM, /*faultPresent=*/true, /*clearFailed=*/true);
}

// --- OTA screen ---------------------------------------------------------------
// The wording on this screen comes from OtaOutcome (lib/ota), so the scenes
// drive a REAL one rather than pushing literal strings at GlobalState: a change
// to a message shows up here without anyone remembering to copy it.
//
// This mirrors ESPCommsManager::publishOutcome(), which lives in src/ and is not
// in the renderer's source list. It is the one duplicated thing in these scenes;
// keep the two in step.
// nowMs matches this scene's OtaOutcome calls (they run at t=0 through the
// build-up of a scenario) into GlobalState::m_otaProgressAtMs, which is what
// drawOTA() compares against millis() (also 0 here -- the host clock never
// advances in a scene) to decide whether the transfer line is stale. Default
// 0 keeps every existing call site fresh (0 - 0 = 0, well inside
// kRateStaleMs) without having to touch them.
void publishOutcome(Rig& r, const OtaOutcome& o, unsigned long nowMs = 0) {
  // setOTA() FIRST: it wipes the bus's OTA text and byte counts so a retry
  // cannot inherit the last attempt's words. ESPCommsManager orders it the same
  // way, and anything a scene wants in the byte counters has to be set after.
  r.gs->setOTA();
  r.gs->setOtaText(o.headline(), o.detail());
  r.gs->setOtaContextLine(o.contextLine());
  r.gs->setOtaProgressMs(nowMs);
  switch (o.result()) {
  case OtaResult::InProgress:
    r.gs->setOtaStatus((o.phase() == OtaPhase::Downloading ||
                        o.phase() == OtaPhase::Finishing)
                           ? OTA_DOWNLOADING
                           : OTA_CHECKING);
    break;
  case OtaResult::Success:  r.gs->setOtaStatus(OTA_SUCCESS);   break;
  case OtaResult::UpToDate: r.gs->setOtaStatus(OTA_NO_UPDATE); break;
  default:                  r.gs->setOtaStatus(OTA_FAILED);    break;
  }
}

// Aug 2026: version pair + a live transfer line. Real elapsed virtual time
// (advanceMockMicros(), not a hand-picked timestamp) so that
// GlobalState::getOtaProgressMs() -- published at the true millis() this
// scene finishes at -- reads as FRESH once pump()'s later ticks move the
// clock on a little further, exactly like a real device's onProgress()
// publish does relative to drawOTA()'s next poll.
void sc_otaDownloading(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(millis());
  o.noteCurrentVersion("v1.0.5");
  o.notePhase(OtaPhase::Checking, millis());
  o.noteVersion("v1.0.6");
  o.notePhase(OtaPhase::Downloading, millis());
  // 8400 bytes every 100 ms = 84000 B/s once the EWMA has settled - the same
  // shape as TransferDetailMatchesTheOwnersExample in test/test_otaoutcome,
  // so the rendered rate is a number that suite has already pinned rather
  // than one this scene invents on its own.
  const unsigned long total = 1572864;  // elstft.bin, the measured size
  unsigned long done = 0;
  for (int i = 0; i < 20; ++i) {
    advanceMockMicros(100000);
    done += 8400;
    o.noteProgress(done, total, millis());
  }
  publishOutcome(r, o, millis());
  r.gs->setOTAContentLength((int)total);
  r.gs->setOTABytes((int)done);
}

// "A marginal signal is visible before the download commits" - the owner's
// own wording. Mid-CONNECTING, before anything has been asked of GitHub yet.
void sc_otaConnecting(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(millis());
  o.noteSignal(-61);
  publishOutcome(r, o, millis());
}

void sc_otaChecking(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(0);
  o.notePhase(OtaPhase::Checking, 0);
  publishOutcome(r, o);
}

void sc_otaNoUpdate(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(0);
  o.notePhase(OtaPhase::Checking, 0);
  o.noteVersion(FIRMWARE_VERSION);
  o.upToDate(10);
  publishOutcome(r, o);
}

// The whole reason this work exists: a failure that cannot be mistaken for a
// success. Fault-coloured, empty bar, and it holds for 30 s instead of three.
void sc_otaFailed(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(0);
  o.fail(OtaResult::NoNetwork, 10);
  publishOutcome(r, o);
}

// The stall this project has actually measured (modem sleep left on, ~18%). Its
// detail line is too wide for the one label the OTA screen has, so this is the
// scene that shows what the headline fallback looks like.
void sc_otaStalled(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(0);
  o.notePhase(OtaPhase::Downloading, 0);
  o.noteProgress(283000, 1572864, 10);
  o.fail(OtaResult::DownloadStalled, 30010);
  publishOutcome(r, o);
  r.gs->setOTAContentLength(1572864);
  r.gs->setOTABytes(283000);
}

// The post-reboot confirmation, restored from RTC memory by
// ESPCommsManager::beginBootNotice(). The version number IS the message.
void sc_otaUpdated(Rig& r) {
  baseState(r);
  OtaOutcome o;
  o.begin(0);
  o.noteVersion(FIRMWARE_VERSION);
  o.succeed(10);
  OtaNotice n = o.snapshot();
  OtaOutcome restored;
  restored.restore(n, 0);
  publishOutcome(r, restored);
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
  // The stepper-alarm modal, one image per variant.
  { "alarm",                  sc_alarm,               THEME_DARK,  false, 0 },
  { "alarm-released",         sc_alarmReleased,       THEME_DARK,  false, 0 },
  { "alarm-clearing",         sc_alarmClearing,       THEME_DARK,  false, 0 },
  { "alarm-failed",           sc_alarmFailed,         THEME_DARK,  false, 0 },
  // Light palette. Same two states as their dark counterparts above, so the
  // pair is directly comparable.
  { "light-rest-metric-feed", sc_restMetricFeed,      THEME_LIGHT, false, 0 },
  { "light-overlay-mode",     sc_overlayMode,         THEME_LIGHT, false, 0 },
  { "light-menu-sync-blocked",sc_menuSyncBlocked,     THEME_LIGHT, false, 0 },
  // The modal is red-on-red furniture over a hazard fill, and the light
  // palette's ground is white: worth its own look rather than assuming the
  // dark one transfers.
  { "light-alarm",            sc_alarm,               THEME_LIGHT, false, 0 },
  // OTA screen (a separate screen, not the dashboard).
  { "ota-connecting",         sc_otaConnecting,       THEME_DARK,  false, 0 },
  { "ota-downloading",        sc_otaDownloading,      THEME_DARK,  false, 0 },
  { "ota-checking",           sc_otaChecking,         THEME_DARK,  false, 0 },
  { "ota-no-update",          sc_otaNoUpdate,         THEME_DARK,  false, 0 },
  // The three the old screen could not tell apart from each other, or from a
  // success: a failure, the measured 18% stall, and the post-reboot proof.
  { "ota-failed",             sc_otaFailed,           THEME_DARK,  false, 0 },
  { "ota-stalled",            sc_otaStalled,          THEME_DARK,  false, 0 },
  { "ota-updated",            sc_otaUpdated,          THEME_DARK,  false, 0 },
  // Wi-Fi setup path: the parameterless Display, no dashboard at all.
  { "wifi-setup",             nullptr,                THEME_DARK,  true,  0 },
  { "wifi-connected",         nullptr,                THEME_DARK,  true,  1 },

  // Both themes: the splash is the one screen every user sees on every boot,
  // and it is drawn on the stored palette, so both have to be looked at.
  { "splash",                 nullptr,                THEME_DARK,  false, 0, true },
  { "light-splash",           nullptr,                THEME_LIGHT, false, 0, true },
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

  if (def->splashScreen) {
    // No init() and no pump(): showSplash() draws the whole screen itself, and
    // update() would walk a dashboard object tree that was never built. This is
    // the device's own boot order -- splash first, dashboard after -- stopped
    // at the splash.
    r.display->showSplash();
    for (int i = 0; i < 6; i++) {
      advanceMockMicros(40000);
      lv_timer_handler();
    }
    return true;
  }

  r.display->init();
  def->apply(r);
  pump(r.display);
  return true;
}

}  // namespace shot
