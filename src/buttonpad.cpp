#include <config.h>

#ifdef ELS_USE_BUTTON_ARRAY
#include "buttonpad.h"

#include <config.h>
#include <globalstate.h>

// Mk2 button panel glue (docs/ux-redesign.md Sec. 1-6).
//
// The old file dispatched one handler per verb, each gated on GlobalButtonLock.
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
// answer to "stop it now", not a mode that prevents keys working. GlobalButtonLock
// itself still exists because lib/display still reads it; it is deleted with the
// display rebuild.

ButtonPad::ButtonPad(Spindle* spindle, Leadscrew* leadscrew, KeyArray* pad)
  : m_spindle(spindle),
  m_leadscrew(leadscrew),
  m_pad(pad),
  m_ui() {
  // ButtonPad is heap-allocated (`new ButtonPad` in main.cpp) and the heap is
  // NOT zeroed, so every member is initialised explicitly - see CLAUDE.md
  // ("Constructors must initialise all members"); relying on implicit zeroing
  // has shipped real bugs in this repo.

  // GlobalState still constructs with the pad LOCKED, and nothing toggles the
  // lock any more: this feature set removed the LOCK key with the rest of the
  // Sec. 7 lock model. Left alone the device would boot showing the padlock
  // forever and KeyArray::updateEncoderPos() (src/keyarray.cpp:102) would
  // swallow every encoder click. Clear it once here. Remove this line together
  // with GlobalButtonLock / drawLocked() when the display is rebuilt.
  GlobalState::getInstance()->setButtonLock(GlobalButtonLock::LK_UNLOCKED);
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
  // ELS_ENABLE_BUTTON is intentionally absent: ENABLE is machine state, not a
  // focus target, and is handled by enableHandler() before we get here.
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
  return ctx;
}

void ButtonPad::handle() {
  const unsigned long now = millis();

  // Drain the KeyArray ring buffer rather than taking one event per pass. The
  // DisplayTask sleeps 100 ms (main.cpp), and a single tap is three events
  // (Press/Click/Release), so one-per-pass would stretch every tap over 300 ms
  // and let a burst of presses queue up behind it. Draining changes neither the
  // events nor their order - the ring buffer is FIFO - it just stops the poll
  // rate from spacing them out. Bounded by the buffer size (10) so a key held
  // against a chattering contact can never spin this loop.
  for (int i = 0; i < 10; i++) {
    ButtonInfo press = m_pad->consumeButton();
    if (press.buttonState == BS_NONE) {
      break;
    }

    if (press.button == ELS_ENABLE_BUTTON) {
      enableHandler(press);
      continue;
    }

    UiKey key;
    UiKeyEvent ev;
    if (!codeToKey(press.button, key) || !stateToEvent(press.buttonState, ev)) {
      continue;
    }

    applyIntent(m_ui.handleKey(key, ev, buildContext(), now));
  }

  // Must run every pass, not only when a key arrived: this is what expires the
  // focus back to Jog after UiState::kFocusTimeoutMs of no input (Sec. 1).
  m_ui.tick(now);
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
  //   * handleTimer() (src/keyarray.cpp:68-75) re-reads the matrix, and if a
  //     second key is touched during a hold it emits the cancellation against
  //     button 0 rather than the arrow, so the arrow's Release never arrives.
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
    // harmless - the intermediate mode's slot is only read, never written.
    // Replace with a real DecFeedMode() if the cycle ever stops being 3 long.
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

  // --- Not yet implemented -------------------------------------------------
  case UiIntent::ZeroDro:
    // No-op: the manual-zero datum store does not exist yet. lib/dro resolves a
    // datum from the stops, but nothing owns "the operator zeroed here"
    // (docs/ux-redesign.md Sec. 8, rule 1). Owned by the DRO feature set.
    break;

  case UiIntent::MenuNext:
  case UiIntent::MenuPrev:
  case UiIntent::MenuActivate:
  case UiIntent::CloseMenu:
    // No-ops: UiState already tracks menuOpen()/menuIndex(), but there is no
    // menu UI to draw and no tile actions to run (Units, Theme, DRO datum, Jog
    // speed, Sync, Software update, Setup/Wi-Fi, Diagnostics, About -
    // docs/ux-redesign.md Sec. 6). Owned by the menu feature set, which will
    // dispatch on menuIndex() here.
    break;
  }
}

void ButtonPad::enableHandler(ButtonInfo press) {
  // Unchanged from the Mk1 panel apart from the lock check: ENABLE toggles
  // MM_ENABLED <-> MM_DECELLERATE (Sec. 5). Deliberately outside UiState - it
  // is machine state, not a focus target, so it neither reads nor moves focus.
  GlobalState* globalState = GlobalState::getInstance();
  GlobalMotionMode motionMode = globalState->getMotionMode();

  if (press.buttonState == BS_CLICKED) {
    if (motionMode == GlobalMotionMode::MM_ENABLED) {
      globalState->setMotionMode(GlobalMotionMode::MM_DECELLERATE);
    }
    if (motionMode == GlobalMotionMode::MM_DISABLED) {
      globalState->setMotionMode(GlobalMotionMode::MM_ENABLED);
    }
  }
}

#endif
