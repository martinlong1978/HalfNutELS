#include <config.h>

#ifdef ELS_USE_BUTTON_ARRAY
#include "buttonpad.h"

#include <config.h>
#include <globalstate.h>
// MenuTile (the index -> tile mapping, shared with the carousel that renders
// it) and menuTileBlock() (the availability rule, likewise shared so the
// screen and this file cannot disagree) live in lib/ui/uistate.h, pulled in
// via buttonpad.h. display.h is still needed here for Display itself - the
// Theme and DRO datum tiles call Display::setTheme()/setDroDatum().
#include <display.h>
#include "alarm.h"
#include "WebSettings.h"
#include "setupmode.h"
// commsManager, for UiIntent::AckOta -> ESPCommsManager::acknowledgeOutcome().
#include "ESPCommsManager.h"

// The one Display, built by main.cpp's setup(). Reached by extern rather than
// injected because ButtonPad is constructed FIRST there (Display's constructor
// takes &keyPad->ui()), so there is no ordering in which it could arrive
// through the constructor. Always null-checked: it is null until setup() gets
// to it, and ButtonPad::handle() only ever runs from the DisplayTask, which is
// started long after.
extern Display* display;

// Mk2 button panel glue (docs/ux-redesign.md Sec. 1-6).
//
// The old file dispatched one handler per verb, each gated on a button lock.
// It is now one path: matrix code -> UiKey, ButtonState -> UiKeyEvent, ask
// UiState, execute the single UiIntent it returns.
//
// The event vocabulary comes straight off KeyArray (src/keyarray.cpp:144-170):
//   short press:  Press -> Click -> Release
//   long  press:  Press -> Hold  -> Release   (no Click after a Hold)
// UiState depends on exactly that stream, so events are passed through
// verbatim - never filtered, coalesced or synthesised here.
//
// The lock is gone from this path (docs/ux-redesign.md Sec. 7): HALT is the
// answer to "stop it now", not a mode that prevents keys working. The lock
// state, its enum, and its accessors on GlobalState have since been deleted
// outright now that lib/display no longer reads it either.

ButtonPad::ButtonPad(Spindle* spindle, Leadscrew* leadscrew, KeyArray* pad)
  : m_spindle(spindle),
  m_leadscrew(leadscrew),
  m_pad(pad),
  m_ui() {
  // ButtonPad is heap-allocated (`new ButtonPad` in main.cpp) and the heap is
  // NOT zeroed, so every member is initialised explicitly - see CLAUDE.md
  // ("Constructors must initialise all members"); relying on implicit zeroing
  // has shipped real bugs in this repo. Every member is in the init list above:
  // the three pointers, and m_ui, whose own constructor initialises its fields.
  //
  // There is no settings copy to seed any more. The old constructor rebuilt an
  // entire LatheConfig here from LatheConfigDerived's accessors, because
  // saveLatheSettings() wrote sizeof(LatheConfig) and any field missed by that
  // list would have been written back as a compiled-in default - overwriting
  // the user's commissioned geometry. Geometry is now web-only and carried
  // through flash by saveLathePreferences(), so the copy, the seeding, and the
  // size assert that guarded it are all gone (see buttonpad.h).
}

bool ButtonPad::codeToKey(int code, UiKey& key) {
  switch (code) {
  case ELS_MODE_BUTTON:  key = UiKey::Mode;  return true;
  case ELS_RATE_BUTTON:  key = UiKey::Rate;  return true;
  case ELS_STOPS_BUTTON: key = UiKey::Stops; return true;
  case ELS_LEFT_BUTTON:  key = UiKey::Left;  return true;
  case ELS_OK_BUTTON:    key = UiKey::Ok;    return true;
  case ELS_RIGHT_BUTTON: key = UiKey::Right; return true;
  case ELS_HALT_BUTTON:  key = UiKey::Halt;  return true;
  case ELS_MENU_BUTTON:  key = UiKey::Menu;  return true;
  // ENABLE is now part of the focus model rather than being intercepted ahead
  // of it. It still does exactly what it did - the ToggleEngage arm of
  // applyIntent() is the old enableHandler() body verbatim - but WHETHER it
  // toggles is now UiState's decision (dismiss first, engage second), which is
  // a decision that has to be host-testable.
  case ELS_ENABLE_BUTTON: key = UiKey::Enable; return true;
  default: return false;
  }
}

bool ButtonPad::stateToEvent(int buttonState, UiKeyEvent& ev) {
  switch (buttonState) {
  case BS_PRESSED:  ev = UiKeyEvent::Press;   return true;
  case BS_CLICKED:  ev = UiKeyEvent::Click;   return true;
  case BS_HELD:     ev = UiKeyEvent::Hold;    return true;
  case BS_RELEASED: ev = UiKeyEvent::Release; return true;
  // BS_NONE is the empty-queue marker; BS_DOUBLE_CLICKED is declared but never
  // emitted by KeyArray. Neither has a UiKeyEvent, so drop them.
  default: return false;
  }
}

UiContext ButtonPad::buildContext() {
  GlobalState* globalState = GlobalState::getInstance();
  const GlobalMotionMode motionMode = globalState->getMotionMode();

  UiContext ctx;
  ctx.leftStopSet =
    m_leadscrew->getStopPositionState(LeadscrewStopPosition::LEFT) !=
    LeadscrewStopState::UNSET;
  ctx.rightStopSet =
    m_leadscrew->getStopPositionState(LeadscrewStopPosition::RIGHT) !=
    LeadscrewStopState::UNSET;
  // Engaged: the leadscrew is following the spindle.
  ctx.motionEnabled = (motionMode == GlobalMotionMode::MM_ENABLED);
  // Under power at all - engaged feed, powered run to a stop, interactive jog,
  // or decelerating out of any of those.
  ctx.motionActive = (motionMode != GlobalMotionMode::MM_DISABLED &&
    motionMode != GlobalMotionMode::MM_UNSET);
  // Only menuTileBlock() reads this, for the Sync tile. Sampled from the same
  // GlobalState, at the same instant, as the two motion flags - so the block
  // UiState computes and the one activateMenuTile() recomputes below can only
  // disagree if the machine actually changed in between, which is the race that
  // check exists to catch.
  const GlobalFeedMode feedMode = globalState->getFeedMode();
  ctx.threadMode = (feedMode == FM_THREAD || feedMode == FM_THREAD_REVERSE);
  // The stepper alarm. Latched fault OR reset pulse in flight - both inhibit
  // the machine and both keep the modal up, so UiState sees one flag (see
  // UiContext in lib/ui/uistate.h). Sampled from GlobalState like the rest,
  // where the alarm task publishes it; ButtonPad never touches the AlarmMonitor
  // except to ask for a clear.
  ctx.alarm = globalState->alarmActive();
  // The OTA screen. True for the whole of GlobalState::hasOTA()'s span -
  // connecting/checking/downloading/settled, not only a settled failure - see
  // the long comment on UiContext::ota in lib/ui/uistate.h and the UiStateOta
  // block in test/test_uistate/test_uistate.cpp for why. ButtonPad never talks
  // to ESPCommsManager to READ this; GlobalState is the one bus both tasks
  // already use (see the "Cross-task state" note in CLAUDE.md).
  ctx.ota = globalState->hasOTA();
  return ctx;
}

void ButtonPad::handle() {
  const unsigned long now = millis();

  // Drain the KeyArray ring buffer rather than taking one event per pass. The
  // DisplayTask sleeps 100 ms (main.cpp), and a single tap is three events
  // (Press/Click/Release), so one-per-pass would stretch every tap over 300 ms
  // and let a burst of presses queue up behind it. Draining changes neither the
  // events nor their order - the ring buffer is FIFO - it just stops the poll
  // rate from spacing them out. Bounded by KeyArray::kRingSize so a key held
  // against a chattering contact can never spin this loop - and bounded by the
  // ACTUAL capacity, so a full ring drains in one pass rather than leaving the
  // remainder to wait another 100 ms.
  for (int i = 0; i < KeyArray::kRingSize; i++) {
    ButtonInfo press = m_pad->consumeButton();
    if (press.buttonState == BS_NONE) {
      break;
    }

    UiKey key;
    UiKeyEvent ev;
    if (!codeToKey(press.button, key) || !stateToEvent(press.buttonState, ev)) {
      continue;
    }

    applyIntent(m_ui.handleKey(key, ev, buildContext(), now));
  }

  // The rotary encoder, fed through the identical path. KeyArray no longer acts
  // on it at all (it used to call GlobalState::next/prevFeedPitch() directly,
  // which is how the knob stepped the pitch from inside a widget); it just
  // counts detents, and each one becomes one UiKey::EncoderCw/Ccw Click.
  //
  // A Click and nothing else: a detent is instantaneous, so there is no Press
  // to pair a Release with and nothing to hold. This is not the forbidden kind
  // of synthesis - the HAZARD note below warns against inventing a Release for
  // a key that is still physically down, and a detent asserts nothing about any
  // key's physical state.
  //
  // One handleKey per detent rather than a single stepped-by-N call, so a spin
  // saturates at the ends of the pitch table exactly as repeated key presses
  // would, and so each step is gated by a fresh context. The loop is bounded:
  // consumeEncoderDelta() clamps what one pass can return.
  int detents = m_pad->consumeEncoderDelta();
  while (detents > 0) {
    applyIntent(m_ui.handleKey(UiKey::EncoderCw, UiKeyEvent::Click,
                               buildContext(), now));
    detents--;
  }
  while (detents < 0) {
    applyIntent(m_ui.handleKey(UiKey::EncoderCcw, UiKeyEvent::Click,
                               buildContext(), now));
    detents++;
  }

  // Must run every pass, not only when a key arrived: this is what expires the
  // focus back to Jog after UiState::kFocusTimeoutMs of no input (Sec. 1), and
  // it is also the ONLY place close-on-motion can be guaranteed to happen -
  // motion frequently starts with no key event at all (the web UI, a
  // spindle-driven feed, the natural end of a run), and a picker must not be
  // left on screen over a moving carriage until the operator next touches the
  // panel. Hence the fresh context, built the same way as the ones above.
  m_ui.tick(buildContext(), now);
}

void ButtonPad::applyIntent(UiIntent intent) {
  GlobalState* globalState = GlobalState::getInstance();

  switch (intent) {
  case UiIntent::None:
    break;

  // --- Jog: dead-man, held-to-move (Sec. 3) --------------------------------
  //
  // HAZARD (pre-existing, deliberately NOT fixed here): a continuous jog is
  // stopped only by the Release of the same key, and src/keyarray.cpp can drop
  // that Release outright.
  //   * handleRelease() returns early inside its 10 ms debounce
  //     (src/keyarray.cpp:159), so a very short tap can emit Press with no
  //     matching Release at all.
  //   * handleTimer() (src/keyarray.cpp:68-75) re-reads the matrix one second
  //     after the Press. If a second key is touched by then the scan returns a
  //     different code, so it takes the else branch and clears buttonState to
  //     {0, BS_NONE} WITHOUT emitting anything. The eventual handleRelease()
  //     then emits BS_RELEASED against that cleared `button` - i.e. button 0,
  //     not the arrow - and codeToKey() drops it. The arrow's Release never
  //     arrives.
  //     (The same else branch also fires with no second key if the contact is
  //     open at the one-second mark, which is what makes the first bullet
  //     terminal: the stale BS_PRESSED that would otherwise have produced a
  //     late Release is wiped, silently, one second in.)
  // A dead-man jog that never receives its Release cannot stop itself; only
  // HALT / ENABLE / the next arrow gesture will.
  //
  // This is not new - today's FM_JOG path has the identical exposure - and both
  // candidate fixes are behaviour changes that need the owner's approval:
  //   (a) cap continuous jog with a watchdog in this loop, stopping the
  //       carriage if no Release has arrived within N ms of the Press; or
  //   (b) poll the matrix directly from the display loop (KeyArray already
  //       exposes getCodeFromArray()) and treat "key no longer down" as the
  //       release, making the interrupt stream advisory rather than
  //       authoritative.
  // Neither is implemented. Do not "fix" this by synthesising a Release into
  // UiState - it reasons over the real key stream and would then latch a jog
  // that is still physically held.
  case UiIntent::JogLeftStart:
    globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    globalState->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT);
    break;
  case UiIntent::JogRightStart:
    globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    globalState->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT);
    break;

  // --- The stepper alarm modal ---------------------------------------------
  case UiIntent::ClearAlarm:
    // OK on the alarm modal. This does NOT dismiss anything: it asks the alarm
    // task to pulse the driver's ENABLE line, and the modal comes down only if
    // the fault has actually gone (src/alarm.cpp, lib/alarm/alarmmonitor.h).
    // Nothing is restarted either - motion stays at MM_DISABLED and the sync
    // state stays SS_UNSYNC, so the operator jogs and re-syncs deliberately,
    // which is the whole reason the dialog says so.
    alarmRequestClear();
    break;

  // --- The OTA screen -------------------------------------------------------
  case UiIntent::AckOta:
    // OK on the OTA screen. Like ClearAlarm, this does NOT dismiss anything:
    // it only reaches OtaOutcome::acknowledge() through commsManager, which
    // lets the OTA task's own exitAction() release the modal early instead of
    // waiting out kAckTimeoutMs (lib/ota/otaoutcome.h). UiState emits this
    // unconditionally on every OK click while ctx.ota is true, regardless of
    // phase - acknowledge() is documented safe to call before the outcome has
    // even settled.
    commsManager.acknowledgeOutcome();
    break;

  case UiIntent::JogStop:
  case UiIntent::CancelMotion:
    // Same action for both: HALT and letting go of an arrow both mean "wind the
    // carriage down now". MM_DECELLERATE is idempotent, which is what lets
    // UiState fire CancelMotion on both the Press and the Click of a HALT tap.
    globalState->setMotionMode(GlobalMotionMode::MM_DECELLERATE);
    break;

  // --- Powered run to a stop (Sec. 3) --------------------------------------
  case UiIntent::RunToLeftStop:
    globalState->setMotionMode(GlobalMotionMode::MM_JOG_LEFT);
    globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    break;
  case UiIntent::RunToRightStop:
    globalState->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
    globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    break;

  // --- RATE widget ---------------------------------------------------------
  // setTargetPitchMM() unconditionally, not only when the index moved:
  // next/prevFeedPitch() saturate at the ends of the table, and the leadscrew's
  // target must track the displayed pitch even on a no-op step.
  case UiIntent::PitchNext:
    globalState->nextFeedPitch();
    m_leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());
    break;
  case UiIntent::PitchPrev:
    globalState->prevFeedPitch();
    m_leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());
    break;

  // --- JOG SPEED widget ----------------------------------------------------
  // Jog speed now has its own focus (opened by OK at rest, Sec. 3) instead of
  // riding on next/prevFeedPitch() when the mode happened to be FM_JOG.
  case UiIntent::JogSpeedNext:
    globalState->incJogSpeed();
    break;
  case UiIntent::JogSpeedPrev:
    globalState->decJogSpeed();
    break;

  // --- MODE widget ---------------------------------------------------------
  case UiIntent::ModeNext:
    globalState->IncFeedMode();
    m_leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());
    break;
  case UiIntent::ModePrev:
    // GlobalState has no DecFeedMode(). The cycle is three long
    // (FEED -> THREAD -> THREAD_REVERSE -> FEED, globalstate.cpp:51-75), so
    // stepping forward twice lands on the previous mode. IncFeedMode() also
    // restores that slot's remembered pitch index, and doing it twice is
    // harmless - but NOT because the intermediate slot is untouched: it does
    // get written, since setFeedSelect() always mirrors back into
    // m_pitchMemory (globalstate.cpp:142-143). It is harmless because that
    // write-back is idempotent - it stores the value it just read from the
    // same slot, and every pair (intermediate, destination) either shares one
    // slot or touches two disjoint ones, so nothing leaks between them.
    // Replace with a real DecFeedMode() if the cycle ever stops being 3 long,
    // or if setFeedSelect()'s bounds fallback ever becomes slot-dependent.
    globalState->IncFeedMode();
    globalState->IncFeedMode();
    m_leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());
    break;

  // --- STOPS widget (Sec. 4) -----------------------------------------------
  // UiState has already applied the click-sets / hold-clears asymmetry and the
  // "machine must be at rest" inhibit; there is nothing left to decide here.
  case UiIntent::SetLeftStop:
    m_leadscrew->setStopPosition(LeadscrewStopPosition::LEFT);
    break;
  case UiIntent::ClearLeftStop:
    m_leadscrew->unsetStopPosition(LeadscrewStopPosition::LEFT);
    break;
  case UiIntent::SetRightStop:
    m_leadscrew->setStopPosition(LeadscrewStopPosition::RIGHT);
    break;
  case UiIntent::ClearRightStop:
    m_leadscrew->unsetStopPosition(LeadscrewStopPosition::RIGHT);
    break;

  case UiIntent::ClearBothStops:
    // STOPS held for a second inside the STOPS widget (Sec. 4). UiState has
    // already applied the same at-rest gate as every other stop edit, checked
    // it a second time against a fresh context when the hold fired, and made
    // sure at least one stop existed to clear.
    //
    // Two calls, not a bulk API: unsetStopPosition() is where the helix
    // re-anchor lives (LeadscrewStopSync::unsetStop, leadscrew.cpp:56-96) and
    // it must run for each stop. Clearing LEFT first re-anchors onto RIGHT, and
    // clearing RIGHT then leaves syncPositionState UNSET with no anchor at all
    // - which is the correct end state for "no stops", and precisely why this
    // gesture is the one with a confirm bar in front of it.
    m_leadscrew->unsetStopPosition(LeadscrewStopPosition::LEFT);
    m_leadscrew->unsetStopPosition(LeadscrewStopPosition::RIGHT);
    break;

  // --- DRO DATUM widget (Sec. 6 / Sec. 8) ----------------------------------
  // The picker the DRO datum tile now opens, instead of the tile toggling the
  // value blind behind the carousel. UiState has decided WHICH end; this only
  // persists and applies it.
  case UiIntent::DroDatumLeft:
    setDroDatumPreference(DroDatumPreference::Left);
    break;
  case UiIntent::DroDatumRight:
    setDroDatumPreference(DroDatumPreference::Right);
    break;

  // --- ENABLE (Sec. 5) -----------------------------------------------------
  case UiIntent::ToggleEngage:
    enableHandler();
    break;

  // --- Not yet implemented -------------------------------------------------
  case UiIntent::ZeroDro:
    // No-op: the manual-zero datum store does not exist yet. lib/dro resolves a
    // datum from the stops, but nothing owns "the operator zeroed here"
    // (docs/ux-redesign.md Sec. 8, rule 1). Owned by the DRO feature set.
    break;

  // --- MENU (Sec. 6) -------------------------------------------------------
  case UiIntent::MenuActivate:
    activateMenuTile();
    break;

  case UiIntent::MenuNext:
  case UiIntent::MenuPrev:
  case UiIntent::CloseMenu:
    // Nothing to execute. Moving through the carousel and closing it are pure
    // UI state, which UiState already holds (menuOpen()/menuIndex()) and the
    // display already renders from - no machine state changes, so there is no
    // action for this method to take.
    break;
  }
}

// The nine tiles of docs/ux-redesign.md Sec. 6.
//
// UiState decided WHEN this runs and WHICH tile is selected; MenuTile (declared
// in lib/ui/uistate.h, shared with the carousel that renders it) decides what
// that index MEANS. This method only executes.
//
// THE FOCUS CHANGE IS NOT HERE. By the time this runs, UiState has already
// closed the carousel and moved focus to menuTileDestination(tile) - Jog for
// the tiles whose result is visible on the main screen, the tile's own widget
// or screen for the rest. This method owns the SIDE EFFECTS only, which is why
// the Jog speed tile no longer has to replay two synthetic keystrokes to move
// focus and why several tiles below now have nothing left to do at all.
//
// m_ui.menuIndex() is read AFTER the menu has closed. That is a documented
// contract on UiState::menuIndex(), not an accident: the index is reset when
// the menu OPENS, never when it closes, precisely so the caller can dispatch on
// it here. It is pinned by UiStateMenu.ActivateLeavesTheIndexReadableToCaller.
//
// Nothing here may be slow or blocking beyond what is already unavoidable: it
// runs on the DisplayTask, which is where every other button action runs, and
// the two genuinely expensive tiles (the flash write behind Theme/DRO datum,
// and the reboot behind Wi-Fi setup) are precisely the ones gated on the
// carriage being stopped.
void ButtonPad::activateMenuTile() {
  GlobalState* globalState = GlobalState::getInstance();

  // The availability gate. Sampled fresh, from GlobalState, at the moment of
  // the press - not from the UiContext that produced the intent, which was
  // built for UiState's own decision and could be a keypress older by the time
  // a queued burst drains.
  //
  // "Under power" is the same predicate UiContext::motionActive carries and the
  // same one saveLathePreferences() refuses on: motionMode is neither MM_DISABLED
  // nor MM_UNSET, so it covers the engaged feed, the powered run to a stop, the
  // interactive jog AND the deceleration tail.
  const GlobalMotionMode motionMode = globalState->getMotionMode();
  const bool motionActive = (motionMode != GlobalMotionMode::MM_DISABLED &&
                             motionMode != GlobalMotionMode::MM_UNSET);
  const GlobalFeedMode feedMode = globalState->getFeedMode();
  const bool threadMode =
    (feedMode == FM_THREAD || feedMode == FM_THREAD_REVERSE);

  const int tile = m_ui.menuIndex();
  if (menuTileBlock(tile, motionActive, threadMode) != MTB_NONE) {
    // Refused. Silent HERE by design, not silent to the operator: the display
    // evaluates the identical menuTileBlock() call every tick, so the tile the
    // press landed on is already drawn dim and the hint row already reads
    // "stop the carriage first" / "needs thread mode" - and the right-hand hint
    // has already dropped "OK open". Popping a transient message on top of a
    // permanent one that says the same thing would be noise, and this build has
    // no animation to dismiss it with (Sec. 8).
    //
    // This is now the SECOND evaluation, not the first: UiState applies the
    // same menuTileBlock() to decide whether to close the carousel at all, and
    // on a refusal it never emits MenuActivate, so ordinarily this branch is
    // unreachable. It stays because it is the AUTHORITATIVE one - UiState is
    // judging a UiContext, this is judging the machine itself, sampled at the
    // moment of execution - and it is what catches motion starting in the
    // microseconds between the two. Reaching it means the menu has already
    // closed and focus has already moved; nothing here can put that back, and
    // nothing needs to, because the destination for every blockable tile
    // (Theme, DRO datum, Sync, Software update, Wi-Fi setup) is a screen that
    // shows the unchanged value. Do NOT delete this check to "avoid the
    // duplication" - the duplication is one shared rule evaluated against two
    // different snapshots, which is the whole point.
    return;
  }

  switch ((MenuTile)tile) {
  case MENU_UNITS:
    // Toggle mm <-> inch. NO setFeedSelect(-1) alongside it: setUnitMode() now
    // restores that unit's remembered pitch index itself (globalstate.cpp), and
    // resetting the index here would throw that memory away every time - the
    // exact defect Sec. 4 has this mechanism to fix. The leadscrew's target has
    // to follow, because the restored index is generally a different pitch.
    globalState->setUnitMode(
      globalState->getUnitMode() == METRIC ? IMPERIAL : METRIC);
    m_leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());
    break;

  case MENU_THEME: {
    // Read what is actually STORED, toggle that, persist, and only then apply.
    //
    // Read from flash, not from a copy: flash is the authority for what is
    // persisted, and re-reading is what makes it impossible for this tile to
    // write back a value the user never chose. It costs one small SPI read on
    // the DisplayTask, immediately before an erase that costs a thousand times
    // more. If nothing valid is stored, readLathePreferences() answers with
    // the defaults and the save below refuses anyway.
    //
    // Both preferences are read because saveLathePreferences() takes both -
    // the datum has to be passed through unchanged, or persisting a theme
    // would revert it.
    uint8_t theme = THEME_DARK;
    DroDatumPreference datum = DroDatumPreference::Left;
    (void)readLathePreferences(theme, datum);

    // Persist FIRST, apply second. saveLathePreferences() can refuse, and a
    // display that had already switched theme would then be showing a setting
    // the machine did not keep - it would silently revert on the next boot.
    const uint8_t next = toggleTheme(theme);
    if (!saveLathePreferences(next, datum)) {
      // Refused. Nothing to undo: no state changed anywhere, because the value
      // being toggled came from flash rather than from anything held here.
      //
      // The refusal that matters is "the carriage is under power" - a 4 KB
      // sector erase disables the instruction cache on both cores and stalls
      // the spindle loop for tens of milliseconds - and reaching it here is a
      // RACE, not the normal path: the gate above tests the same condition, so
      // motion must have started in the microseconds since. The operator sees
      // the tile go dim on the next display tick, because that same condition
      // is now true there too. (saveLathePreferences() also refuses when flash
      // holds no settings blob this firmware recognises, which cannot happen on
      // a machine that got as far as running: main.cpp would have entered AP
      // setup instead.)
      break;
    }
    if (display != nullptr) {
      display->setTheme(next);
    }
    break;
  }

  case MENU_DRO_DATUM:
    // Nothing to execute. The tile used to toggle the datum in place, behind
    // the carousel - a flash write whose only visible effect was on the travel
    // bar the menu was covering, which is precisely the "OK does nothing
    // visible" complaint this feature set exists to answer. It now OPENS the
    // picker instead: UiState has already moved focus to UiFocus::DroDatum, its
    // arrows emit DroDatumLeft/DroDatumRight, and applyIntent() persists the
    // chosen end through setDroDatumPreference().
    break;

  case MENU_JOG_SPEED:
    // "The same widget OK opens at rest; here for discoverability" (Sec. 6).
    // Nothing to execute: UiState has already moved focus to UiFocus::JogSpeed,
    // via menuTileDestination(), so the two synthetic MENU + OK keystrokes this
    // used to replay through handleKey() are gone. Every tile now gets its
    // destination the same way, from one table, instead of this one tile
    // steering focus by hand.
    break;

  case MENU_SYNC:
    // "I am in the groove here" - anchors the helix on the current (carriage,
    // spindle-phase) pair so an existing thread can be picked up.
    //
    // The gate above is load-bearing, not decoration: setSyncPoint() zeroes the
    // following error and raises SS_SYNC, which releases Leadscrew::update()'s
    // re-sync gate. Called while that gate is holding the axis, the carriage
    // lurches up to a full pitch (measured 0.32 mm) into the work, and there is
    // a residual lost-update race on m_expectedPosition that is reachable only
    // in that state. Sec. 6 words this as "against a stopped spindle"; the
    // enforceable form of it is the AXIS being disengaged, which is what the
    // menuTileBlock() call above requires - the re-sync gate can only be
    // holding while the leadscrew is under power. It also refuses the whole
    // tile in feed mode, where a thread anchor means nothing.
    m_leadscrew->setSyncPoint();
    break;

  case MENU_SOFTWARE_UPDATE:
    // Replaces the old half-nut hold, which the Mk2 panel removed - this tile
    // is now the ONLY route to an OTA update. Setting the flag is the whole
    // action: the SpindleTask's timerCallback() sees hasOTA() and runs
    // commsManager.loop() instead of the motion path, which spawns the 24 KB OTA
    // task, and the display swaps to the OTA screen (main.cpp, ESPCommsManager).
    globalState->setOTA();
    break;

  case MENU_WIFI_SETUP:
    // DOES NOT RETURN - this reboots. Sets the RTC-memory flag and restarts, and
    // main.cpp's setup() then enters AP/config mode on the way back up
    // (src/setupmode.h). Like OTA, this is now the only runtime route in; the
    // boot-time gesture (hold OK at power-on) remains as the fallback.
    requestSetupOnNextBoot();
    break;

  case MENU_DIAGNOSTICS:
    // Nothing to execute. UiState has moved focus to UiFocus::Diagnostics and
    // lib/display renders it, reading Leadscrew / Spindle directly from the
    // DisplayTask (Sec. 6: live position error and pulse counts). There is no
    // machine state to change, which is the whole nature of the screen.
    //
    // The motion-trace capture is NOT armed from here - it has its own tile
    // (MENU_DEBUG_CAPTURE, below), which is blocked under power because it
    // allocates. This tile stays a pure read-only screen. The two do meet on
    // screen: the capture's status replaces this screen's title while a trace
    // exists, which is why the Debug capture tile lands the operator here.
    break;

  case MENU_ABOUT:
    // Nothing to execute, for the same reason: UiState has moved focus to
    // UiFocus::About and the screen reads firmware version (include/version.h),
    // IP and uptime for itself.
    break;

  case MENU_DEBUG_CAPTURE: {
    // Arm or abandon a motion-trace capture (lib/global_state/debugcapture.h).
    //
    // ONE KEY, TWO ANSWERS, and which one you get is on screen before you
    // press it: UiState has already moved focus to UiFocus::Diagnostics, whose
    // title row IS the capture's status line. Press once from a resting state
    // to arm; press again while it is recording (or holding a trace) to throw
    // it away and go back to idle.
    //
    // ARMING MUST HAPPEN BEFORE THE CUT. That is not a convention, it is the
    // only order the machine allows: menuTileBlock() refuses this tile while
    // the carriage is under power, and the panel is inert under power anyway
    // (the motion lockout, uistate.h). A capture armed afterwards would also
    // simply not contain the cut.
    //
    // The gate above is doing real work here, exactly as it is for Theme:
    // setDebugMode() calls malloc/free for ~100 KB, and the heap walk that
    // implies is not something to run while the spindle loop is generating
    // steps.
    GlobalState* gs = GlobalState::getInstance();
    const DebugCaptureState state = gs->debug().state();
    if (state == DBG_SENDING) {
      // THE ONE STATE THIS KEY MUST NOT TOUCH. The upload task is reading the
      // trace right now; freeing it under that read is a use-after-free on a
      // 100 KB buffer. The upload ends by itself (DBG_SENT or DBG_FAILED),
      // and both of those are pressable. Nothing on screen changes, which is
      // honest: "SENDING TRACE" is still exactly what is happening.
      break;
    }
    const bool idle = (state == DBG_OFF || state == DBG_SENT ||
                       state == DBG_NOMEM);
    // Arm from idle; otherwise abandon - a capture still recording, or a full
    // trace waiting to go, or a failed upload still holding its trace.
    // setDebugMode(false) stops the hot loop before it frees, so this is safe
    // to press at any moment the tile is reachable at all.
    gs->setDebugMode(idle);
    break;
  }

  case MENU_TILE_COUNT:
  default:
    // Unreachable: UiState clamps menuIndex() to [0, kMenuItemCount - 1], and
    // the static_assert beside MenuTile ties that count to this list.
    break;
  }
}

// Persist and apply one end of the travel as the DRO datum. Same read-persist-
// apply order, and the same reasons for each step, as the Theme tile: flash is
// the authority, the save can refuse, and a display that had already switched
// would then be showing a setting the machine did not keep.
//
// The IDEMPOTENT short-circuit is the point of the absolute DroDatumLeft/Right
// intents (see the DroDatum arrow branch in lib/ui/uistate.cpp): pressing LEFT
// when the datum is already Left must not cost a 4 KB sector erase, which
// disables the instruction cache on both cores and stalls the spindle loop for
// tens of milliseconds. A next/prev toggle could not make that check, because
// by construction it always names a different value.
void ButtonPad::setDroDatumPreference(DroDatumPreference wanted) {
  uint8_t theme = THEME_DARK;
  DroDatumPreference datum = DroDatumPreference::Left;
  (void)readLathePreferences(theme, datum);

  if (datum == wanted) {
    // Already there. Nothing to write, nothing to apply - and the operator sees
    // the choice they pressed stay selected, which is the correct feedback.
    return;
  }
  // Theme is read and passed straight back through unchanged: the save takes
  // both preferences, so persisting a datum with a stale theme would revert it.
  if (!saveLathePreferences(theme, wanted)) {
    // Refused. Nothing to undo - the value being changed came from flash, not
    // from anything held here. The refusal that matters is "the carriage is
    // under power", and reaching it is a RACE: menuTileBlock() refuses the tile
    // that opens this widget under power, so motion must have started since.
    return;
  }
  if (display != nullptr) {
    display->setDroDatum(wanted);
  }
  // Sec. 8 also has re-picking the datum CLEAR a manual zero. Not implemented,
  // and deliberately not faked: there is no manual-zero store anywhere yet
  // (drawTravel() hard-codes manualZeroSet false, and UiIntent::ZeroDro is
  // still a no-op above), so there is nothing to clear. Whoever adds that store
  // owns adding the clear here.
}

void ButtonPad::enableHandler() {
  // The toggle itself: MM_ENABLED <-> MM_DECELLERATE (Sec. 5). Byte for byte
  // what this method has always done, minus the BS_CLICKED test - that has
  // moved, unchanged in meaning, into UiState, which only emits ToggleEngage on
  // a Click. Everything else is preserved deliberately:
  //   * motionMode is read ONCE, up front, and both `if`s test that snapshot,
  //     so a single press can never transition twice.
  //   * any other mode (MM_DECELLERATE, MM_JOG_*, MM_INTERACTIVE_JOG_*,
  //     MM_UNSET) is a no-op. ENABLE does not interrupt a powered run or a jog;
  //     HALT does.
  // What is NEW is the decision about whether to get here at all - with a
  // widget or the menu open, UiState returns None and closes it instead (first
  // press dismisses, second engages).
  GlobalState* globalState = GlobalState::getInstance();
  GlobalMotionMode motionMode = globalState->getMotionMode();

  if (motionMode == GlobalMotionMode::MM_ENABLED) {
    globalState->setMotionMode(GlobalMotionMode::MM_DECELLERATE);
  }
  if (motionMode == GlobalMotionMode::MM_DISABLED) {
    globalState->setMotionMode(GlobalMotionMode::MM_ENABLED);
  }
}

#endif
