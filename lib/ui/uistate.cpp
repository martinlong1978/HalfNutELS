// UI focus state machine for the Mk2 button panel (docs/ux-redesign.md §1-§6).
//
// Pure C++: no Arduino / ESP / FreeRTOS / LVGL includes, no heap, no clock of
// its own (time arrives as `nowMs`). Every decision is a pure function of
// (key, event, UiContext) plus the small amount of state declared in uistate.h.
//
// The whole file leans on one fact about the real keypad
// (src/keyarray.cpp:144-170):
//
//     short press:  Press -> Click -> Release
//     long  press:  Press -> Hold  -> Release      (no Click after a Hold)
//
// So for any one gesture the state machine sees the surrounding Press/Release
// as well as the Click or Hold in the middle. Exactly one of those events may
// act; the others must be inert, or a single tap fires twice - or worse, the
// Release aborts the powered run that its own Click just started.
#include "uistate.h"

// Out-of-line definitions for the in-class-initialised static constants, so
// odr-use (binding to a const reference, e.g. inside EXPECT_EQ) links under
// pre-C++17 as well.
const unsigned long UiState::kFocusTimeoutMs;
const int UiState::kMenuItemCount;

namespace {

// Arrow direction as a sign: -1 left, +1 right. Only ever called for the two
// arrow keys.
inline int arrowDir(UiKey key) { return key == UiKey::Left ? -1 : +1; }

// The four selector widgets, i.e. every focus that is neither Jog nor Menu.
// These are the ones that commit on OK and expire on the idle timeout.
inline bool isWidgetFocus(UiFocus f) {
  return f == UiFocus::JogSpeed || f == UiFocus::Rate || f == UiFocus::Mode ||
         f == UiFocus::Stops;
}

// --- The powered-run latch, m_runPhase ------------------------------------
//
// UiState has to keep its own record that a run to a stop is in flight, and
// that record cannot be replaced by ctx.motionActive, nor cleared whenever
// ctx.motionActive is false. Both fail, in opposite directions:
//
//   * Purely derived from the context: the caller cannot have moved the
//     machine, let alone reported it, by the time the very next event arrives.
//     An arrow click inside that window would start a SECOND run instead of
//     cancelling the first.
//   * Cleared on every inactive context: the Release of the very tap that
//     started the run arrives inside that same window, so the latch would be
//     wiped before it ever meant anything and a run could never be cancelled.
//
// Hence two phases. A run is Commanded when we emit the intent, and becomes
// Confirmed once some context has actually reported motionActive. Only a
// Confirmed run can be inferred to have ENDED: it ended by itself at the stop,
// which is the case UiState is never told about (defect 2). A Commanded run
// may not have started yet, so an inactive context says nothing about it and
// the latch is left alone.
//
// A naturally-completed run and an in-flight one present IDENTICAL contexts at
// the decision point (motionActive false, in the first case because it is over
// and in the second because it has not begun) and demand opposite answers, so
// the phase is the only thing that separates them. Do not collapse it.
//
// No direction is stored. This used to be a signed m_runToStopDir whose sign
// was documented as the direction, but nothing ever read the sign: both
// consumers test only "is a run in flight", the cancel fires on either arrow
// regardless of which one started the run, and the RunToLeft/RightStop intent
// is chosen from the key in hand. The phase is the whole of the state.

}  // namespace

UiState::UiState()
    : m_focus(UiFocus::Jog),
      m_menuOpen(false),
      m_menuIndex(0),
      m_lastActivityMs(0),
      m_runPhase(RunPhase::None),
      m_jogDir(0) {}

UiFocus UiState::focus() const { return m_focus; }

bool UiState::menuOpen() const { return m_menuOpen; }

int UiState::menuIndex() const { return m_menuIndex; }

UiIntent UiState::handleKey(UiKey key, UiKeyEvent ev, const UiContext& ctx,
                            unsigned long nowMs) {
  // Any key event - even an inert one - counts as activity and restarts the
  // idle clock (§1).
  m_lastActivityMs = nowMs;

  // -------------------------------------------------------------------------
  // HALT: checked before focus, before overlays, before anything (§5).
  // Safety-critical, so it fires on the earliest event of the gesture (Press)
  // as well as on Click and Hold. CancelMotion is idempotent, so a short press
  // cancelling on both Press and Click is harmless. Release must not re-fire.
  // -------------------------------------------------------------------------
  if (key == UiKey::Halt) {
    if (ev == UiKeyEvent::Release) {
      return UiIntent::None;
    }
    m_menuOpen = false;
    m_focus = UiFocus::Jog;
    m_runPhase = RunPhase::None;
    m_jogDir = 0;
    return UiIntent::CancelMotion;
  }

  // -------------------------------------------------------------------------
  // Dead-man jog terminator, second only to HALT.
  //
  // A hold-to-jog is only safe if letting go ALWAYS stops it. That has to be
  // true no matter what changed between the Press and the Release - focus, an
  // open menu, a stop appearing, the leadscrew being engaged - because every
  // one of those would otherwise hit an early `return None` further down and
  // leave m_jogDir set with the carriage still moving and no JogStop ever
  // emitted. So the release of EITHER arrow ends an in-flight jog, and it does
  // so before any other consideration.
  //
  // Not direction-matched on purpose. The matrix scan reports one key at a
  // time, so a release of the opposite arrow mid-jog should not occur; if a
  // malformed event stream produces one anyway, stopping is the safe reading.
  // Erring towards "stop" can at worst cut a jog short; erring the other way
  // runs the carriage into the chuck.
  // -------------------------------------------------------------------------
  if ((key == UiKey::Left || key == UiKey::Right) &&
      ev == UiKeyEvent::Release && m_jogDir != 0) {
    m_jogDir = 0;
    return UiIntent::JogStop;
  }

  // -------------------------------------------------------------------------
  // Reconcile the powered-run latch against the machine, before anything reads
  // it (§7). A run does not only end because the operator cancelled it - it
  // also ends BY ITSELF when the carriage reaches the stop, and nothing
  // reports that to UiState. Left unreconciled, the latch outlives the run and
  // the next arrow click returns CancelMotion for a run that is already over:
  // the click is silently eaten. Run to the stop, click back, nothing happens.
  //
  // So: a Confirmed run plus an inactive machine means the run is over, and
  // the latch goes. See the two-phase note above for why a Commanded run is
  // deliberately left standing.
  //
  // KNOWN LIMIT, and the reason UiContext's comment insists on a FRESH context:
  // a run that is never Confirmed is never reconciled either. If no key event
  // between the start of a run and its natural end ever sees motionActive, the
  // latch stays Commanded for good, and the eaten-click symptom above returns.
  // In practice the Release of the starting tap is that event - but only if the
  // caller re-reads the machine after executing the intent it was just handed.
  // A caller that snapshots one context per 10 Hz display poll and replays it
  // over a batch of queued key events defeats this entirely. It cannot be
  // repaired from inside UiState: a Commanded latch and a completed one are
  // indistinguishable here, and clearing on a timer would break the pinned
  // "a powered run survives the focus timeout" contract (see tick()).
  //
  // This sits here, at the top of the ladder, rather than next to the latch's
  // only reader in the Jog branch, for two reasons. Every path that consults
  // the latch is below it, so there is no route that can read a stale one. And
  // a run can finish while the operator is somewhere else entirely - inside
  // the menu, in the STOPS widget, engaged - all of which return early further
  // down; reconciling here means those keypresses still clean up, instead of
  // the staleness surviving until the operator happens to press an arrow.
  //
  // It is deliberately BELOW the dead-man jog terminator. That terminator must
  // stay the first thing after HALT and must stay unconditional: letting go of
  // an arrow has to stop the carriage no matter what else is true. Bookkeeping
  // never goes above it.
  // -------------------------------------------------------------------------
  if (ctx.motionActive) {
    if (m_runPhase == RunPhase::Commanded) {
      m_runPhase = RunPhase::Confirmed;
    }
  } else if (m_runPhase == RunPhase::Confirmed) {
    m_runPhase = RunPhase::None;
  }

  // -------------------------------------------------------------------------
  // MENU: top level, so it opens over any widget and closes from anywhere (§6).
  // Acts on Click only - the Press and Release of the same tap would otherwise
  // toggle the menu straight back shut.
  // -------------------------------------------------------------------------
  if (key == UiKey::Menu) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;
    }
    if (m_menuOpen) {
      m_menuOpen = false;
      m_focus = UiFocus::Jog;
      return UiIntent::CloseMenu;
    }
    m_menuOpen = true;
    m_focus = UiFocus::Menu;
    m_menuIndex = 0;  // the carousel is not resumable; every open starts at 0
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // Menu open: the carousel owns the arrows and OK. MODE / RATE / STOPS must
  // not steal focus out from under it - the menu leaves on MENU or HALT only,
  // and both of those were handled above.
  // -------------------------------------------------------------------------
  if (m_menuOpen) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;  // Press / Release / Hold are all inert here
    }
    if (key == UiKey::Ok) {
      return UiIntent::MenuActivate;  // tiles toggle in place; menu stays open
    }
    if (key == UiKey::Left) {
      // Saturating, not wrapping. A blocked move returns None so the caller can
      // treat any non-None result as "redraw".
      if (m_menuIndex <= 0) {
        return UiIntent::None;
      }
      --m_menuIndex;
      return UiIntent::MenuPrev;
    }
    if (key == UiKey::Right) {
      if (m_menuIndex >= kMenuItemCount - 1) {
        return UiIntent::None;
      }
      ++m_menuIndex;
      return UiIntent::MenuNext;
    }
    return UiIntent::None;  // MODE / RATE / STOPS ignored while the menu is up
  }

  // -------------------------------------------------------------------------
  // Selector keys: a Click moves focus to that widget and nothing else. A
  // repeat of the same selector is a no-op that merely restarts the idle timer
  // (§1 lists the leave conditions as OK / HALT / idle - not the key itself).
  // -------------------------------------------------------------------------
  if (key == UiKey::Mode || key == UiKey::Rate || key == UiKey::Stops) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;
    }
    m_focus = (key == UiKey::Mode)   ? UiFocus::Mode
              : (key == UiKey::Rate) ? UiFocus::Rate
                                     : UiFocus::Stops;
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // OK: three jobs that never overlap (§1). Press and Release are always inert
  // so the same gesture cannot act twice.
  // -------------------------------------------------------------------------
  if (key == UiKey::Ok) {
    if (ev == UiKeyEvent::Click) {
      if (isWidgetFocus(m_focus)) {
        m_focus = UiFocus::Jog;  // commit and dismiss
        return UiIntent::None;
      }
      // At rest: open the jog-speed widget, the setting the arrows already
      // drive. No intent - opening a widget is pure UI state.
      m_focus = UiFocus::JogSpeed;
      return UiIntent::None;
    }
    if (ev == UiKeyEvent::Hold && m_focus == UiFocus::Jog) {
      // Defined "at rest" only, so a slow OK press inside a widget can never
      // silently move the datum. Zeroing moves no metal, so the MM_ENABLED
      // inhibit of §3 does not apply.
      return UiIntent::ZeroDro;
    }
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // Arrows. What they do depends entirely on the current focus.
  // -------------------------------------------------------------------------
  if (key != UiKey::Left && key != UiKey::Right) {
    return UiIntent::None;  // unreachable today; keeps the switch total
  }
  const bool left = (key == UiKey::Left);

  switch (m_focus) {
    case UiFocus::JogSpeed:
      if (ev != UiKeyEvent::Click) {
        return UiIntent::None;  // discrete steps only; Hold has no meaning here
      }
      return left ? UiIntent::JogSpeedPrev : UiIntent::JogSpeedNext;

    case UiFocus::Rate:
      if (ev != UiKeyEvent::Click) {
        return UiIntent::None;
      }
      return left ? UiIntent::PitchPrev : UiIntent::PitchNext;

    case UiFocus::Mode:
      if (ev != UiKeyEvent::Click) {
        return UiIntent::None;
      }
      return left ? UiIntent::ModePrev : UiIntent::ModeNext;

    case UiFocus::Stops: {
      // No stop edits while the carriage is under power - ONE rule, both
      // directions, both arrows. The machine must be at rest to change a stop.
      //
      // Clearing is the dangerous half. `hitLeftEndstop()` is
      // `leftStopState == SET && pos <= leftStopPosition`
      // (leadscrew_stopsync.h:98), and that predicate is the ONLY thing that
      // arrests an MM_ENABLED feed (leadscrew.cpp:239-240). Clear the stop you
      // are cutting towards and the arrest is not delayed, it is deleted: the
      // carriage feeds into the chuck. Clearing the OTHER stop is quieter but
      // still destructive - LeadscrewStopSync::unsetStop (leadscrew.cpp:56-96)
      // re-anchors the helix onto the survivor through a truncating
      // `(int)(delta / ratio)`, so it silently shifts the thread phase for the
      // next pass; and if no stop survives, syncPositionState goes UNSET, the
      // anchor is gone, and the thread can never be re-synced for a second cut.
      //
      // Setting is unsafe for different reasons. Setting the stop you are
      // feeding into makes hitEndstop() true on the very next update, so the
      // feed slams to MM_DISABLED mid-thread with the tool still in the cut.
      // Setting the far stop instead latches the spindle sync anchor
      // (setStop(), leadscrew.cpp:104-108) and redefines the thread datum.
      // And it cannot mean what it says regardless: DisplayTask sleeps 100 ms
      // (main.cpp:75), so "here" is up to a tenth of a second stale - metres
      // per minute of carriage travel - and a stop in the wrong place is worse
      // than no stop, because the operator will trust it on the next pass.
      //
      // No workflow in the spec needs it: §4's gestures and §6's Sync tile are
      // all at-rest operations. The cost of the rule is one press of ENABLE.
      //
      // motionEnabled alone is NOT enough, and the gap is not academic. It
      // means MM_ENABLED only, while a powered run to a stop is MM_JOG_LEFT /
      // MM_JOG_RIGHT - so for the whole duration of a run the engaged inhibit
      // is false and every edit above is live. Three ordinary presses (click
      // an arrow to start the run, press STOPS, hold the same arrow) then
      // clear the stop the carriage is travelling towards under power, which
      // is that run's ONLY arrest (leadscrew.cpp:233); unsetStop writes
      // INT32_MIN, so the run does not overshoot, it never terminates.
      // motionActive is the broader signal - engaged feed, powered run,
      // interactive jog, deceleration - and gates the edits the same way.
      if (ctx.motionEnabled || ctx.motionActive) {
        return UiIntent::None;  // display says "stop the carriage to edit stops"
      }

      // The deliberate asymmetry of §4: setting a stop is cheap to undo,
      // clearing one loses a position you may have spent time finding. So a
      // click only ever SETS, and only a hold CLEARS. Each arrow sees its own
      // stop and nothing else.
      const bool stopSet = left ? ctx.leftStopSet : ctx.rightStopSet;
      if (ev == UiKeyEvent::Click) {
        if (stopSet) {
          return UiIntent::None;  // display flashes "hold to clear"
        }
        return left ? UiIntent::SetLeftStop : UiIntent::SetRightStop;
      }
      if (ev == UiKeyEvent::Hold) {
        if (!stopSet) {
          return UiIntent::None;  // a hold must never set a stop by accident
        }
        return left ? UiIntent::ClearLeftStop : UiIntent::ClearRightStop;
      }
      return UiIntent::None;  // Press / Release inert
    }

    case UiFocus::Menu:
      // Unreachable: m_menuOpen and Menu focus are kept in lockstep and the
      // menu branch above already returned.
      return UiIntent::None;

    case UiFocus::Jog:
      break;
  }

  // --- Focus is Jog: the arrows drive the carriage (§3). -------------------

  // Inhibited entirely while the leadscrew is engaged. The state bar says why.
  if (ctx.motionEnabled) {
    return UiIntent::None;
  }

  const int dir = arrowDir(key);

  // A powered run is in flight - either confirmed, or commanded and still
  // inside the poll window; the reconciliation above has already discarded any
  // run that ended by itself. The next discrete gesture on EITHER arrow
  // cancels it (§7 - cancellable by any of three keys). The opposite arrow
  // cancels rather than reversing: one gesture, one effect. Press and Release
  // stay inert, so the Release of the cancelling tap does not also fire, and
  // so the Press of a fresh tap cannot cancel before its own Click is seen.
  if (m_runPhase != RunPhase::None) {
    if (ev == UiKeyEvent::Click || ev == UiKeyEvent::Hold) {
      m_runPhase = RunPhase::None;
      return UiIntent::CancelMotion;
    }
    return UiIntent::None;
  }

  const bool stopSet = (dir < 0) ? ctx.leftStopSet : ctx.rightStopSet;

  if (stopSet) {
    // Click-to-run. Hold behaves identically (§3 table) and is safe because no
    // Click follows a Hold. Press/Release must be inert: a Release must never
    // abort the powered run its own Click just started.
    if (ev == UiKeyEvent::Click || ev == UiKeyEvent::Hold) {
      // Commanded, not yet confirmed: the caller has not had a chance to move
      // the machine, let alone report it. The reconciliation above promotes
      // this to Confirmed on the first context that says motionActive.
      m_runPhase = RunPhase::Commanded;
      return (dir < 0) ? UiIntent::RunToLeftStop : UiIntent::RunToRightStop;
    }
    return UiIntent::None;
  }

  // No stop on this side: dead-man jog, bounded by the physical gesture.
  // "Continuous jog while held" is Press/Release - the Click and Hold that
  // arrive in between are redundant and must be inert.
  //
  // Only the Press is handled here. Release is owned by the unconditional
  // terminator at the top of handleKey, which has already run: reaching this
  // point with a Release means m_jogDir was 0, i.e. no jog to stop.
  if (ev == UiKeyEvent::Press) {
    m_jogDir = dir;
    return (dir < 0) ? UiIntent::JogLeftStart : UiIntent::JogRightStart;
  }
  return UiIntent::None;
}

bool UiState::tick(unsigned long nowMs) {
  // Only the selector widgets expire. Jog is the rest state (nothing to fall
  // back to) and Menu is exempt: it leaves on MENU or HALT only (§1).
  if (!isWidgetFocus(m_focus)) {
    return false;
  }
  if (nowMs - m_lastActivityMs < kFocusTimeoutMs) {
    return false;
  }
  m_focus = UiFocus::Jog;
  // Motion is deliberately untouched: the idle timeout moves focus and nothing
  // else, so a powered run survives it and the next arrow click still cancels.
  return true;
}
