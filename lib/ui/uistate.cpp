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
const unsigned long UiState::kStopsConfirmMs;

namespace {

// Arrow direction as a sign: -1 left, +1 right. Only ever called for the two
// arrow keys.
inline int arrowDir(UiKey key) { return key == UiKey::Left ? -1 : +1; }

// The selector widgets: the small pickers that sit over the rest screen. These
// are the ones that commit on OK and expire on the idle timeout.
//
// DroDatum joins the original four, and joining is the whole of its wiring -
// OK-commits-and-dismisses, the ENABLE dismiss and tick()'s 4 s expiry all key
// off this predicate, so the only code DroDatum needs of its own is the arrow
// branch that produces its intents. That is the point of modelling it on Mode.
inline bool isWidgetFocus(UiFocus f) {
  return f == UiFocus::JogSpeed || f == UiFocus::Rate || f == UiFocus::Mode ||
         f == UiFocus::Stops || f == UiFocus::DroDatum;
}

// The read-only full screens (Diagnostics, About). Deliberately NOT widgets:
// they are exempt from the idle timeout for the reasons set out on the UiFocus
// enum, and their arrows are inert. They leave on OK, MENU or HALT.
inline bool isScreenFocus(UiFocus f) {
  return f == UiFocus::Diagnostics || f == UiFocus::About;
}

// THE ONE FOCUS THAT DELIBERATELY SURVIVES MOTION (owner ruling):
//
//   "the diagnostic screen should survive Enable and Jog, rather than
//    dismissing like the others. It could be genuinely useful to have that on
//    screen when debugging. Ok should clear it."
//
// Every other focus is dismissed the moment the carriage moves, on the
// principle that the operator's attention belongs on the tool. Diagnostics is
// the exception because its content is only meaningful WHILE the machine runs:
// following error, pulse counts and the sync anchor all read zero at rest, so a
// screen that closes on motion can never show the thing it exists to show.
//
// About is NOT included. It is a static version/credits page with no live
// content and no reason to be up under power, so it keeps the ordinary rule.
//
// Consequences, all three keyed off this one predicate: tick() leaves it alone,
// ENABLE engages instead of dismissing, and OK is exempted from the motion
// lockout so it can still be closed.
inline bool survivesMotion(UiFocus f) { return f == UiFocus::Diagnostics; }

// One detent of the rotary encoder, either way.
inline bool isEncoder(UiKey k) {
  return k == UiKey::EncoderCw || k == UiKey::EncoderCcw;
}

// "The carriage is under power, in any form." The one place the two motion
// flags are combined, so every rule that depends on the machine being at rest
// asks the same question and they can never drift apart.
//
// Both flags, not just motionActive, even though UiContext documents the latter
// as a superset: the OR is what makes the rule correct for any caller, and it
// costs nothing. See the long note in the Stops arrow branch for why
// motionEnabled alone is NOT enough - it misses the powered run to a stop,
// during which clearing the stop being travelled towards deletes that run's
// only arrest.
inline bool underPower(const UiContext& ctx) {
  return ctx.motionEnabled || ctx.motionActive;
}

// No stop may be edited while the carriage is under power - §4's rule, so the
// per-arrow edits and the clear-both gesture can never drift apart.
//
// SUBSUMED by the motion lockout in handleKey(): the STOPS key can no longer
// take focus under power, and the arrows are gated before the Stops branch is
// reached, so every call site below is unreachable from the panel. It stays -
// see the note on the lockout - because it is the enforceable statement of the
// rule at the point where a stop would actually be edited, and because the
// clear-both hold re-checks it against a context a whole second fresher than
// the one that armed the bar.
inline bool stopEditsInhibited(const UiContext& ctx) { return underPower(ctx); }

// The knob does nothing at all while the carriage is under power. Now SUBSUMED
// by the motion lockout, which returns before the encoder branch is reached
// from any panel input; kept because it states the rule at the point of use and
// costs one predicate call on a path that is already returning None.
inline bool encoderInhibited(const UiContext& ctx) { return underPower(ctx); }

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
      m_jogDir(0),
      m_stopsConfirming(false),
      m_stopsPressMs(0),
      m_stopsOpenedByPress(false) {}

UiFocus UiState::focus() const { return m_focus; }

bool UiState::menuOpen() const { return m_menuOpen; }

int UiState::menuIndex() const { return m_menuIndex; }

UiIntent UiState::handleKey(UiKey key, UiKeyEvent ev, const UiContext& ctx,
                            unsigned long nowMs) {
  // Any key event - even an inert one - counts as activity and restarts the
  // idle clock (§1).
  m_lastActivityMs = nowMs;

  // Any event on any OTHER key ends an in-progress clear-both confirm. The
  // matrix reports one key at a time, so this should not normally happen - but
  // it is also the self-healing path for a STOPS Release that KeyArray drops
  // (the debounce and one-second-rescan holes documented in buttonpad.cpp), and
  // without it a dropped Release would leave the bar pinned full on screen for
  // ever. Above HALT deliberately, since HALT returns early.
  if (key != UiKey::Stops) {
    m_stopsConfirming = false;
    // ...and with it the "this press opened the widget" marker, for the same
    // self-healing reason: it describes a STOPS press that is physically down,
    // and an event on any other key means that press is over (or its Release
    // was dropped). This is also what keeps the marker fresh across HALT and
    // the ENABLE dismiss - both force focus to Jog and both return through
    // here, so neither needs a clear of its own.
    m_stopsOpenedByPress = false;
  }

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
  // ENABLE: first press dismisses, second engages (§5, refined).
  //
  // §5 says only "ENABLE toggles MM_ENABLED <-> MM_DECELLERATE, exactly as
  // today", and that is still what the toggle DOES - ButtonPad's ToggleEngage
  // arm is the old enableHandler() verbatim. What changed is when the toggle is
  // reached: engaging is a commitment to start cutting, and it must not happen
  // while the operator's attention is still inside a picker. So with a widget or
  // the menu up, ENABLE is a dismiss and nothing else; you come back to the rest
  // screen, see the state chip, and press it again deliberately.
  //
  // Note the asymmetry this creates and why it is acceptable: DISengaging also
  // costs the extra press. That is not a safety hole, because it is not the only
  // way to stop - HALT is unconditional, works from every focus, and is the key
  // §5 makes "always live" precisely so that stopping never depends on where the
  // UI happens to be. ENABLE is the deliberate one; HALT is the reflex.
  //
  // Click only. Press/Release would double-fire the toggle across one tap, and
  // the old handler in ButtonPad already acted on BS_CLICKED alone.
  //
  // Deliberately BELOW the run-phase reconciliation, so a dismiss still cleans
  // up a run that finished while the widget was open, and ABOVE the menu branch,
  // which swallows every key it does not recognise.
  // -------------------------------------------------------------------------
  if (key == UiKey::Enable) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;
    }
    // survivesMotion() is the exception: on Diagnostics, ENABLE does its real
    // job. Requiring a dismiss first would mean the screen could never be on
    // display for the engagement it is there to instrument - you would have to
    // close it to start the machine, which is precisely backwards.
    if ((m_menuOpen || isWidgetFocus(m_focus) || isScreenFocus(m_focus)) &&
        !survivesMotion(m_focus)) {
      // isScreenFocus too: Diagnostics and About are full screens that hide the
      // rest screen entirely, so the reason the dismiss exists - do not commit
      // to cutting while the operator's attention is somewhere else and the
      // state chip is not on screen - applies to them at least as strongly as
      // it does to a picker.
      m_menuOpen = false;
      m_focus = UiFocus::Jog;
      return UiIntent::None;  // dismiss only - does NOT engage
    }
    return UiIntent::ToggleEngage;
  }

  // -------------------------------------------------------------------------
  // THE MOTION LOCKOUT (OWNER RULING). While the carriage is under power, the
  // only functions the panel still answers are the ones that STOP things.
  //
  //   "every button except halt and enable should be disabled whilst moving.
  //    So it shouldn't be possible to open a menu or tile while moving. When
  //    moving, all of the operator's attention should be on the tool and
  //    workpiece, not the screen/menus."
  //
  // WHERE IT SITS, and why exactly here:
  //   * BELOW HALT and the dead-man jog terminator, which must stay the first
  //     two things in the ladder. Both are stop functions and both are
  //     unconditional; neither may acquire a precondition, least of all one
  //     that is true precisely when the machine is moving.
  //   * BELOW the run-phase reconciliation, so a run that ended by itself is
  //     still retired by whatever key event happens to be in flight - including
  //     the ones this gate is about to swallow. Put the gate above it and every
  //     inert keypress under power stops reconciling, and the eaten-click
  //     symptom the latch exists to prevent comes back the moment the operator
  //     presses anything while the carriage is running.
  //   * BELOW ENABLE, which is "unchanged, exactly as now" by the same ruling:
  //     dismiss on the first press, toggle on the second, under power as at
  //     rest. Disengaging must not be harder than engaging was.
  //   * ABOVE everything else - the encoder, MENU, the menu branch, the
  //     selectors, STOPS, OK and the per-focus arrow switch. One gate, checked
  //     once, instead of the five separate "moving, X disabled" rules this
  //     replaces.
  //
  // THE ARROWS ARE THE ONE REFINEMENT, and it is safety-critical. Disabling
  // them wholesale would delete two gestures whose entire purpose is to stop
  // the machine:
  //   * the dead-man terminator. Already handled above, unconditionally, and
  //     that placement is the reason it is not mentioned again here.
  //   * cancelling a powered run to a stop. Handled below, because the run is
  //     the reason underPower() is true in the first place - a rule that
  //     inhibits the only way to abort a run whenever a run is in progress is
  //     no rule at all.
  // So: the arrows may not START motion, take focus, or step any setting while
  // under power. They may still terminate a jog and cancel a run. Stated in one
  // line: while moving, the only live functions are the ones that stop things.
  // -------------------------------------------------------------------------
  if (underPower(ctx)) {
    // A clear-both confirm cannot survive into motion. The bar is only ever
    // drawn for a gesture that would succeed, and from here it cannot, so it
    // must not stay pinned on screen. The top of handleKey() already does this
    // for every OTHER key; this covers STOPS' own events, which it exempts.
    m_stopsConfirming = false;
    // Same exemption, same fix, for the selector-toggle marker: a STOPS press
    // cannot take focus under power, so nothing here may leave a marker
    // standing that a later Click would read as "my own press opened this".
    m_stopsOpenedByPress = false;

    // OK closes the Diagnostics screen, under power as at rest. The lockout
    // exists so the panel cannot be operated while metal moves; closing a
    // read-only screen operates nothing - it cannot start, stop or alter
    // motion, and it returns the operator to the main readout, which is the
    // direction the lockout wants them going anyway. Without this exemption
    // "OK clears it" would be false in exactly the state the screen is for.
    //
    // Click only, like every other OK branch, so one tap cannot act twice.
    if (key == UiKey::Ok && ev == UiKeyEvent::Click && survivesMotion(m_focus)) {
      m_focus = UiFocus::Jog;
      return UiIntent::None;
    }

    if (key == UiKey::Left || key == UiKey::Right) {
      // The run cancel (§7). Deliberately NOT conditioned on m_focus, unlike
      // the copy in the Jog branch below: a run can only be STARTED from Jog
      // focus and focus cannot be moved under power, so the two are equivalent
      // through the panel - but if a widget is somehow open over a live run
      // (motion started from the web UI while a picker was up), the arrow that
      // stops it must not be the thing that finds itself in the wrong focus.
      // Erring towards "stop" is the same bargain the terminator above makes.
      //
      // Click and Hold only. Press must stay inert or the press of a cancelling
      // tap would fire before its own Click; Release must stay inert or the
      // release of that same tap would fire a second time.
      if (m_runPhase != RunPhase::None &&
          (ev == UiKeyEvent::Click || ev == UiKeyEvent::Hold)) {
        m_runPhase = RunPhase::None;
        return UiIntent::CancelMotion;
      }
    }
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // The rotary encoder. One detent = one discrete step of whatever the current
  // focus owns, with two deliberate departures from what the arrows do:
  //
  //   Focus       Encoder
  //   Jog         PITCH        <- NOT what the arrows do here (they jog). The
  //                               knob fills the gap, so pitch is adjustable
  //                               from the rest screen without pressing RATE.
  //   Rate        pitch
  //   JogSpeed    jog speed
  //   Mode        mode
  //   Menu        move between tiles
  //   Stops       INERT        <- the second departure, and the important one.
  //
  // STOPS is inert BY DESIGN, not by omission. A knob is far easier to nudge
  // than a key is to press - a sleeve, a chip, a knock on the panel - and every
  // gesture in the STOPS widget either destroys a position the operator spent
  // time finding or plants one in a place they never saw. §4 already answers
  // this for the keys by making a click set and only a hold clear; there is no
  // equivalent of "hold" on a knob, so the answer here is that the knob does
  // not touch stops at all. Setting an endstop stays a deliberate keypress.
  //
  // MOTION INHIBIT: while the carriage is under power the knob does NOTHING,
  // in every focus, including the menu. This was the knob's own blanket rule
  // before the motion lockout above generalised it to the whole panel, and the
  // reasoning that justified it there is what the lockout now says once:
  //   * A knob is the input most easily disturbed by accident on a running
  //     machine: a sleeve, a chip, a knock on the panel. A key has to be
  //     pressed; a knob only has to be brushed.
  //   * Nothing the knob emits is ever needed while metal is moving, so the
  //     broad rule costs no capability at all. (The arrows were the exception
  //     that had to be stated separately then, and still are now - they cancel
  //     a powered run and they are the dead-man terminator for a jog.)
  //   * "The knob is dead while the machine is running" is a rule an operator
  //     can hold in their head.
  // encoderInhibited() below is therefore UNREACHABLE from the panel: the
  // lockout returns first, for every focus and every context that would satisfy
  // it. It stays as the local statement of the rule.
  //
  // The predicate is underPower() - motionEnabled OR motionActive - not
  // motionEnabled alone. The engaged feed is not the only state worth
  // protecting: the powered run to a stop, the interactive jog and the
  // deceleration tail are all "the carriage is moving", which is precisely when
  // a stray step of pitch or mode is most costly, and none of them are
  // MM_ENABLED. It is the same predicate the lockout and the stop edits use.
  // -------------------------------------------------------------------------
  if (isEncoder(key)) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;  // one Click per detent; nothing else means a turn
    }
    if (encoderInhibited(ctx)) {
      return UiIntent::None;  // see MOTION INHIBIT above - dead in every focus
    }
    const bool ccw = (key == UiKey::EncoderCcw);

    if (m_menuOpen) {
      // WRAPPING, exactly like the arrows in the menu branch below. Every move
      // succeeds, so every move is a redraw.
      if (ccw) {
        m_menuIndex = (m_menuIndex + kMenuItemCount - 1) % kMenuItemCount;
        return UiIntent::MenuPrev;
      }
      m_menuIndex = (m_menuIndex + 1) % kMenuItemCount;
      return UiIntent::MenuNext;
    }

    switch (m_focus) {
      case UiFocus::Stops:
        return UiIntent::None;  // inert - see the note above
      case UiFocus::JogSpeed:
        return ccw ? UiIntent::JogSpeedPrev : UiIntent::JogSpeedNext;
      case UiFocus::Mode:
        return ccw ? UiIntent::ModePrev : UiIntent::ModeNext;
      case UiFocus::DroDatum:
        // The knob drives what the focus owns, exactly like Mode - anticlockwise
        // for the left-hand end, clockwise for the right. NOT the STOPS
        // exception: that one is inert because every gesture in it destroys a
        // position the operator spent time finding, whereas the datum is a
        // two-way choice that one press of the other arrow puts straight back.
        // (The knob is already dead under power by the blanket inhibit above,
        // and menuTileBlock() refuses the tile that opens this widget under
        // power anyway, so a stray detent can never land mid-cut.)
        return ccw ? UiIntent::DroDatumLeft : UiIntent::DroDatumRight;
      case UiFocus::Diagnostics:
      case UiFocus::About:
        return UiIntent::None;  // read-only screens own nothing to step
      case UiFocus::Jog:
        // The rest screen: the knob is the pitch control. (The motion inhibit
        // is the blanket one above - by here the carriage is at rest.)
        return ccw ? UiIntent::PitchPrev : UiIntent::PitchNext;
      case UiFocus::Rate:
        return ccw ? UiIntent::PitchPrev : UiIntent::PitchNext;
      case UiFocus::Menu:
        // Unreachable: Menu focus and m_menuOpen move together, and the
        // m_menuOpen branch above already returned.
        return UiIntent::None;
    }
    return UiIntent::None;
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
    if (isScreenFocus(m_focus)) {
      // MENU is one of the three ways out of a read-only screen. It CLOSES
      // rather than opening the carousel on top: the operator got here through
      // the menu, so the key that took them in is the obvious one to take them
      // back out, and re-opening the carousel would put a picker over a screen
      // that was itself opened from that picker. CloseMenu, not None - it is
      // still "the MENU key dismissed what was on screen", the caller has
      // nothing to execute for it either way, and a non-None result is the
      // display's redraw hint.
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
      // OK ALWAYS CLOSES THE MENU, and focus goes wherever the result of the
      // tile is visible (menuTileDestination(), uistate.h). This replaces the
      // old "tiles toggle in place; menu stays open", which was the source of
      // the owner's complaint that OK does nothing visible: the carousel is a
      // full-width panel sitting directly on top of the pitch, the theme and
      // the travel bar - the three things the tiles actually change - so every
      // tile that DID fire changed something the operator could not see, and
      // the four that did nothing at all were indistinguishable from it.
      //
      // THE ONE EXCEPTION: a refused tile. It changes nothing and the carousel
      // stays open, because the reason is already on screen - the display
      // evaluates this same menuTileBlock() every tick, so the tile is drawn
      // dim and the hint row reads "stop the carriage first" / "needs thread
      // mode". Closing on a refusal would throw that explanation away at the
      // exact moment the operator needs to read it, and land them on a rest
      // screen where nothing happened for no stated reason.
      //
      // The block is evaluated HERE, from the fresh UiContext, because the
      // destination cannot be chosen without knowing whether the tile fires at
      // all. It is the same shared rule the display dims with and the same one
      // ButtonPad re-checks against GlobalState immediately before executing -
      // NOT a second copy. ButtonPad's check stays: it samples the machine
      // rather than a context, and it is the one that must be authoritative if
      // motion starts in the microseconds between the two.
      if (menuTileBlock(m_menuIndex, ctx.motionActive, ctx.threadMode) !=
          MTB_NONE) {
        return UiIntent::None;
      }
      m_menuOpen = false;
      // m_menuIndex is deliberately left alone - ButtonPad reads it back after
      // this returns to decide which tile to execute (see menuIndex()).
      m_focus = menuTileDestination(m_menuIndex);
      return UiIntent::MenuActivate;
    }
    // WRAPPING, not saturating: "there is nothing worse than cycling through
    // to something you know is at the end". With six tiles the far end is five
    // presses away the wrong way and one press the right way.
    //
    // The +kMenuItemCount before the modulo is what keeps this correct at
    // index 0 - C++ % on a negative left operand yields a negative result, and
    // a negative index would render and dispatch off the front of the tile
    // table. Every move now succeeds, so every move is a redraw and neither
    // arm can return None.
    if (key == UiKey::Left) {
      m_menuIndex = (m_menuIndex + kMenuItemCount - 1) % kMenuItemCount;
      return UiIntent::MenuPrev;
    }
    if (key == UiKey::Right) {
      m_menuIndex = (m_menuIndex + 1) % kMenuItemCount;
      return UiIntent::MenuNext;
    }
    return UiIntent::None;  // MODE / RATE / STOPS ignored while the menu is up
  }

  // -------------------------------------------------------------------------
  // Selector keys: a Click moves focus to that widget - and a second press of
  // the SAME selector closes it again, back to Jog.
  //
  // OWNER RULING, and a change from the first cut: "menu opens the menu,
  // pressing a second time closes it. The same logic should apply to rate, mode
  // and stops." This used to be a no-op that merely restarted the idle timer,
  // on the reading that §1 lists a widget's leave conditions as OK / HALT /
  // idle and not the key itself. That reading made MENU the odd one out of four
  // otherwise identical keys: every other selector could only be un-pressed by
  // waiting four seconds or reaching for a different key. One rule for all four
  // is the smaller thing to remember, and it costs nothing - the key that opened
  // a picker is the most obvious one to shut it.
  //
  // Pressing a DIFFERENT selector still switches focus straight across (Mode ->
  // Rate in one press, not Mode -> Jog -> Rate). Only the selector matching the
  // current focus closes.
  //
  // Click only, as before: Press and Release must stay inert or a single tap
  // would open and close in one gesture.
  // -------------------------------------------------------------------------
  if (key == UiKey::Mode || key == UiKey::Rate) {
    if (ev != UiKeyEvent::Click) {
      return UiIntent::None;
    }
    const UiFocus target = (key == UiKey::Mode) ? UiFocus::Mode : UiFocus::Rate;
    m_focus = (m_focus == target) ? UiFocus::Jog : target;
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // STOPS is a selector like MODE and RATE, but it is also the only selector
  // with a gesture of its own: held, it clears BOTH stops behind a one second
  // confirm bar (§4).
  //
  // That gesture is why STOPS takes focus on the PRESS, where MODE and RATE
  // take it on the Click. The keypad emits no Click after a Hold
  // (src/keyarray.cpp:144-170), so a Click-only selector can never be focused
  // by the same gesture that holds it: holding STOPS from the rest screen used
  // to do nothing whatsoever - no focus change, no bar, no clear - and
  // clear-both was reachable only as tap-then-hold. Moving the focus change one
  // event earlier makes press-and-hold work from the rest screen AND puts the
  // widget on screen while the bar fills, so the operator watches it fill over
  // the stop markers and the travel bar they are about to lose, which is the
  // whole point of having a confirm bar rather than an instant action.
  //
  // MODE and RATE stay Click-only, deliberately. The asymmetry is not "STOPS is
  // special" for its own sake: STOPS is the only selector where the Hold means
  // something, so it is the only one where taking focus on the Click loses a
  // gesture. Acting on Press has a real cost - a press that is never followed
  // by a Click (KeyArray can drop events; see the buttonpad.cpp hazards) still
  // moves focus - and there is nothing to buy with it on a selector whose only
  // job is to open a picker.
  //
  // There is no second timer here, and there must not be one. KeyArray's hold
  // timer is already one second (src/keyarray.cpp:58), so the Hold event IS the
  // one-second gesture - it arrives exactly when the bar should be full. All
  // this branch adds is the press bookkeeping the bar is drawn from.
  //
  // Why clear-both needs the confirm bar when the single-stop clear does not:
  // the per-arrow clear takes one stop and leaves the other, so the helix
  // re-anchors onto the survivor and the setup is recoverable. Clearing both
  // leaves nothing to re-anchor onto - syncPositionState goes UNSET and the
  // thread can never be picked up again for a second cut - so the whole setup
  // is gone, and the operator gets a second of visible progress in which to let
  // go instead.
  // -------------------------------------------------------------------------
  if (key == UiKey::Stops) {
    if (ev == UiKeyEvent::Press) {
      // Take focus HERE, not on the Click - see the note above. The widget is
      // then on screen for the whole of the confirm bar, and the Click that
      // follows an ordinary tap lands on an already-focused widget and changes
      // nothing.
      //
      // Note where this sits: the menu branch above swallows every non-Click
      // event while the carousel is open, so a STOPS press cannot steal focus
      // from the menu (§6: the menu leaves on MENU or HALT only), and cannot
      // arm the bar from there either.
      //
      // Record whether THIS press is the one that opened the widget, before the
      // assignment makes that unknowable. The Click below reads it and declines
      // to toggle the widget shut when it is set - see the member's note in
      // uistate.h. False here means the widget was already open, i.e. this is
      // the second press of the selector, i.e. the Click may close it.
      m_stopsOpenedByPress = (m_focus != UiFocus::Stops);
      m_focus = UiFocus::Stops;

      // Arm the confirm bar - but ONLY if this hold could actually succeed, so
      // the bar never fills for a gesture the machine is going to refuse (§4
      // calls that out for the STOPS hint; menuTileBlock() makes the same
      // point for the menu). Two preconditions, both re-checked on the Hold:
      //   * the carriage is at rest. Same gate as every other stop edit.
      //   * there is at least one stop to clear.
      // The focus precondition this used to carry is gone because it is now
      // trivially satisfied by the line above - opening the widget on the Press
      // is exactly what makes the hold reachable from the rest screen. Focus is
      // still what decides whether the bar can arm at all, though: with the
      // menu open this code never runs.
      m_stopsConfirming = (!stopEditsInhibited(ctx) &&
                           (ctx.leftStopSet || ctx.rightStopSet));
      m_stopsPressMs = nowMs;
      return UiIntent::None;
    }

    if (ev == UiKeyEvent::Hold) {
      const bool armed = m_stopsConfirming;
      m_stopsConfirming = false;  // consumed either way; the bar is done
      if (!armed) {
        return UiIntent::None;
      }
      // Re-check against the FRESH context, not the one the Press saw. A whole
      // second has passed, and the machine can have been started in it - by the
      // web UI, by a spindle-driven feed, by anything. This is the gate that
      // actually matters; the one on the Press only decides whether to draw.
      if (stopEditsInhibited(ctx)) {
        return UiIntent::None;
      }
      if (!ctx.leftStopSet && !ctx.rightStopSet) {
        return UiIntent::None;
      }
      return UiIntent::ClearBothStops;
    }

    if (ev == UiKeyEvent::Release) {
      // Let go before the hold fired: cancel, cleanly and with no intent. This
      // is the escape hatch the confirm bar exists to offer.
      m_stopsConfirming = false;
      // The press is over, so the marker it set has nothing left to describe.
      // This is what makes the NEXT tap a closing one.
      m_stopsOpenedByPress = false;
      return UiIntent::None;
    }

    // Click: the selector toggle, and STOPS' share of the owner's ruling.
    //
    // THE ORDERING IS THE WHOLE DESIGN HERE, so it is worth stating in full.
    // STOPS takes focus on the Press (above), which is what makes press-and-hold
    // from the rest screen work. That leaves two gestures whose Click arrives
    // with the widget already open, and they need OPPOSITE answers:
    //
    //   Jog -> tap STOPS      Press opens, then Click. Must STAY open, or the
    //                         widget would flash and vanish inside one tap and
    //                         the selector could never be entered at all.
    //   Stops -> tap STOPS    Press changes nothing, then Click. Must CLOSE -
    //                         this is the ruling.
    //
    // m_stopsOpenedByPress is what tells them apart: it is exactly "the press
    // currently down is the one that opened this widget".
    //
    // And the reason the close hangs off the Click rather than the Press: a long
    // press emits Press -> Hold -> Release with NO Click (src/keyarray.cpp:
    // 144-170), so closing here is invisible to the hold gesture. Holding STOPS
    // with the widget already open still fills the bar and still fires
    // ClearBothStops - which closing on the Press would have destroyed.
    //
    // The `m_focus == Stops` half also keeps the dropped-Press path working:
    // KeyArray can lose events, and a Click that arrives from another focus
    // without its Press must still OPEN the widget rather than close it.
    if (m_focus == UiFocus::Stops && !m_stopsOpenedByPress) {
      m_focus = UiFocus::Jog;
      // The bar is drawn over the widget, so it cannot outlive it. The Press of
      // this very tap armed it (both stops set, at rest is enough); leaving it
      // set would paint a filling confirm bar over the rest screen for the
      // millisecond until the Release.
      m_stopsConfirming = false;
      return UiIntent::None;
    }
    m_focus = UiFocus::Stops;
    return UiIntent::None;
  }

  // -------------------------------------------------------------------------
  // OK: three jobs that never overlap (§1). Press and Release are always inert
  // so the same gesture cannot act twice.
  // -------------------------------------------------------------------------
  if (key == UiKey::Ok) {
    if (ev == UiKeyEvent::Click) {
      if (isWidgetFocus(m_focus) || isScreenFocus(m_focus)) {
        // Commit and dismiss. DroDatum rides on this branch unchanged: its
        // arrows have already emitted their intent and the caller has already
        // applied it, so "commit" here means the same thing it means for Mode -
        // stop editing, go back and look at the result. The read-only screens
        // have nothing to commit; OK is simply the most obvious way out.
        m_focus = UiFocus::Jog;
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

    case UiFocus::DroDatum:
      // Which end of the travel the DRO reads zero from. Same shape as Mode - a
      // small set of choices, arrows pick, OK commits and dismisses, HALT and
      // the 4 s idle both drop back to Jog - but the intents are ABSOLUTE
      // (DroDatumLeft / DroDatumRight), not a ModeNext/ModePrev pair. Three
      // reasons, and the first is the one that matters:
      //
      //   * With only two choices, a next/prev pair is a TOGGLE, and a toggle
      //     cannot be pressed twice safely: two presses of "next" land back
      //     where they started. So a double-tap, a bouncing contact, or an
      //     operator who is not sure the first press registered (which is the
      //     entire complaint this feature set exists to fix) silently undoes
      //     itself. LEFT means left however many times it is pressed.
      //   * The choice is SPATIAL - it names an end of the carriage travel - so
      //     the left arrow meaning the left-hand end is a mapping the operator
      //     cannot get wrong, and it needs no memory of what is currently
      //     selected to predict.
      //   * It lets the caller skip the write. Persisting the datum is a flash
      //     sector erase (saveLathePreferences()), which stalls both cores for
      //     tens of milliseconds; an absolute intent can be compared against
      //     what is stored and dropped when it matches. A next/prev pair cannot
      //     be - by construction it always names a different value.
      //
      // Click only, like every other widget's arrows: a Hold here has no
      // meaning, and Press/Release must stay inert or one tap acts twice.
      if (ev != UiKeyEvent::Click) {
        return UiIntent::None;
      }
      // Inhibited under power, using the SAME predicate as the stop edits.
      //
      // This is the one place a widget's arrows are inhibited where Rate and
      // Mode's are not, and the difference is real rather than a lapse: a pitch
      // or mode step is a RAM write that takes effect on the next update, which
      // is why §3 insists RATE keeps working mid-cut. Persisting the datum is a
      // flash sector erase, and saveLathePreferences() REFUSES outright while
      // the carriage is under power (src/WebSettings.h) - so without this the
      // press would emit an intent that is certain to be silently dropped one
      // layer down. An offered gesture the machine will ignore is exactly what
      // menuTileBlock() and §4's STOPS hint both exist to prevent; the same
      // reasoning that dims the tile has to hold once the widget is open.
      //
      // Reachable, though rare: menuTileBlock() means the widget can only be
      // OPENED at rest, but motion can start underneath it from the web UI or a
      // spindle-driven feed before the 4 s idle closes it - the same window the
      // clear-both hold re-checks against a fresh context for.
      if (underPower(ctx)) {
        return UiIntent::None;  // display says "stop the carriage first"
      }
      return left ? UiIntent::DroDatumLeft : UiIntent::DroDatumRight;

    case UiFocus::Diagnostics:
    case UiFocus::About:
      // INERT, deliberately, and not "not yet implemented".
      //
      // Paging was considered and rejected. There is one screen of content per
      // spec (§6: position error and pulse counts; version, IP and uptime), so
      // a page intent would be a control with nothing on the other side of it -
      // the decorative-guard pattern this branch has already had to remove
      // three times. Add it when a second page exists, and pin it then.
      //
      // Inert is also the safe reading for these two keys specifically: LEFT
      // and RIGHT are the JOG keys, and they are the ones an operator's hand
      // goes to by reflex. A read-only screen is exactly where a reflex press
      // should do nothing at all.
      return UiIntent::None;

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
      if (stopEditsInhibited(ctx)) {
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
  // UNREACHABLE from the panel since the motion lockout: underPower() is
  // motionEnabled OR motionActive, so any context that satisfies this one has
  // already returned above. Kept as the local statement of §3's rule on the one
  // branch that actually commands carriage motion.
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

int UiState::stopsConfirmPermille(unsigned long nowMs) const {
  if (!m_stopsConfirming) {
    return 0;
  }
  // Unsigned subtraction, so a millis() rollover between the press and the poll
  // yields the correct small elapsed time rather than a huge one - the same
  // trick tick() relies on.
  const unsigned long elapsed = nowMs - m_stopsPressMs;
  // Saturate BEFORE multiplying. On the device unsigned long is 32 bits, and if
  // the Release is ever dropped (see the KeyArray hazards in buttonpad.cpp) this
  // can be asked with a very large elapsed; `elapsed * 1000` would then wrap and
  // the bar would appear to restart. Saturating also covers the ordinary case
  // where the Hold event has not yet been drained by the 100 ms display loop.
  if (elapsed >= kStopsConfirmMs) {
    return 1000;
  }
  return (int)((elapsed * 1000UL) / kStopsConfirmMs);
}

bool UiState::tick(const UiContext& ctx, unsigned long nowMs) {
  // -------------------------------------------------------------------------
  // CLOSE-ON-MOTION (OWNER RULING), checked before the idle timeout because it
  // overrides it: "any open widgets, if they can survive into motion, should
  // probably be closed on motion."
  //
  // The lockout in handleKey() already makes everything on an open widget inert
  // while the carriage moves, and no panel gesture can open one under power -
  // ENABLE dismisses before it engages, the arrows are owned by an open widget
  // so cannot start a jog from inside one, and a run to a stop starts only from
  // Jog focus. So this path is, today, unreachable through the panel.
  //
  // It exists anyway, and deliberately. A reachability argument is only true
  // until someone adds a path, and it fails silently when they do: nothing here
  // would catch it. A picker on screen over a moving carriage is the state this
  // whole feature set exists to forbid, so it is made impossible BY
  // CONSTRUCTION rather than by an argument a future change can invalidate.
  // That is the same call the dead-man terminator makes above - unconditional
  // rather than "safe because nothing can reach it" - and this branch is not
  // free of reachable cases either: motion can start with no key event at all,
  // from the web UI or a spindle-driven feed, while a widget is open.
  //
  // Focus goes to Jog and the carousel closes, so the display falls back to the
  // main readout - travel, position, machine state - which is the information
  // worth having on screen while metal is moving.
  //
  // It touches NOTHING else. m_jogDir and m_runPhase are deliberately not
  // cleared: the dead-man terminator and the run cancel are the two gestures
  // that survive the lockout, and both read state this must not disturb. A jog
  // in flight is precisely a case where this branch runs (the jog IS the
  // motion) and its Release must still emit JogStop afterwards.
  if (underPower(ctx)) {
    // The confirm bar goes with the widget it is drawn over; the gesture behind
    // it is refused under power anyway.
    m_stopsConfirming = false;
    // And the selector-toggle marker, which describes a widget this is about to
    // close. Left standing it would outlive its widget, and the Click of a
    // later tap would read it as "my own press opened this" and decline to
    // close a widget that a dropped Press had never opened.
    m_stopsOpenedByPress = false;
    // The Diagnostics exemption. Deliberately BELOW the two clears above: the
    // confirm bar and the toggle marker belong to gestures that are refused
    // under power regardless of what is on screen, so they go either way.
    if (survivesMotion(m_focus) && !m_menuOpen) {
      return false;  // stays up, and nothing changed, so nothing to redraw
    }
    if (!m_menuOpen && m_focus == UiFocus::Jog) {
      return false;  // already at rest state - no transition, no redraw
    }
    m_menuOpen = false;
    m_focus = UiFocus::Jog;
    return true;
  }

  // Only the selector widgets expire - the five of isWidgetFocus(), DroDatum
  // included. Jog is the rest state (nothing to fall back to), Menu leaves on
  // MENU or HALT only (§1), and Diagnostics / About are read-only screens the
  // operator is meant to be able to WATCH for longer than four seconds; the
  // ruling and its reasoning are on the UiFocus enum in uistate.h.
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
