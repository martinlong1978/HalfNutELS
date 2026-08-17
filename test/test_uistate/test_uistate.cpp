// Host tests for the Mk2 UI focus model (lib/ui/uistate.h).
//
// These pin docs/ux-redesign.md sections 1 (focus model + the OK key table),
// 3 (jog on the bare arrows), 4 (the selector widgets), 5 (HALT and ENABLE)
// and 6 (MENU). Written test-first: lib/ui/uistate.cpp is a stub, so this
// suite is EXPECTED to fail on assertions until the implementer fills it in.
//
// Key event ordering, taken from the real keypad (src/keyarray.cpp:144-170):
//
//   short press:  Press -> Click -> Release
//   long  press:  Press -> Hold  -> Release      (no Click follows a Hold)
//
// That ordering is load-bearing. It is why, with a stop unset, `Left` Press
// starts the jog and `Left` Click must be inert (the Click arrives between the
// Press and the Release of the very same gesture), and why a Hold can act
// directly without double-firing with a Click.

#include <gmock/gmock.h>

#include <ostream>

#include "uistate.h"

// ---------------------------------------------------------------------------
// Pretty-printers, so a failure reads "JogLeftStart vs None" not "4 vs 0".
// Must live in the global namespace for gtest's ADL-based printer to find them.
// ---------------------------------------------------------------------------
std::ostream& operator<<(std::ostream& os, UiFocus f) {
  switch (f) {
    case UiFocus::Jog: return os << "Focus::Jog";
    case UiFocus::JogSpeed: return os << "Focus::JogSpeed";
    case UiFocus::Rate: return os << "Focus::Rate";
    case UiFocus::Mode: return os << "Focus::Mode";
    case UiFocus::Stops: return os << "Focus::Stops";
    case UiFocus::Menu: return os << "Focus::Menu";
  }
  return os << "Focus::<?>";
}

std::ostream& operator<<(std::ostream& os, UiIntent i) {
  switch (i) {
    case UiIntent::None: return os << "None";
    case UiIntent::JogLeftStart: return os << "JogLeftStart";
    case UiIntent::JogRightStart: return os << "JogRightStart";
    case UiIntent::JogStop: return os << "JogStop";
    case UiIntent::RunToLeftStop: return os << "RunToLeftStop";
    case UiIntent::RunToRightStop: return os << "RunToRightStop";
    case UiIntent::CancelMotion: return os << "CancelMotion";
    case UiIntent::PitchNext: return os << "PitchNext";
    case UiIntent::PitchPrev: return os << "PitchPrev";
    case UiIntent::JogSpeedNext: return os << "JogSpeedNext";
    case UiIntent::JogSpeedPrev: return os << "JogSpeedPrev";
    case UiIntent::ModeNext: return os << "ModeNext";
    case UiIntent::ModePrev: return os << "ModePrev";
    case UiIntent::SetLeftStop: return os << "SetLeftStop";
    case UiIntent::ClearLeftStop: return os << "ClearLeftStop";
    case UiIntent::SetRightStop: return os << "SetRightStop";
    case UiIntent::ClearRightStop: return os << "ClearRightStop";
    case UiIntent::ZeroDro: return os << "ZeroDro";
    case UiIntent::MenuNext: return os << "MenuNext";
    case UiIntent::MenuPrev: return os << "MenuPrev";
    case UiIntent::MenuActivate: return os << "MenuActivate";
    case UiIntent::CloseMenu: return os << "CloseMenu";
  }
  return os << "Intent::<?>";
}

std::ostream& operator<<(std::ostream& os, UiKeyEvent e) {
  switch (e) {
    case UiKeyEvent::Press: return os << "Press";
    case UiKeyEvent::Release: return os << "Release";
    case UiKeyEvent::Click: return os << "Click";
    case UiKeyEvent::Hold: return os << "Hold";
  }
  return os << "Event::<?>";
}

namespace {

// Copy so gtest never odr-uses the in-class static constant by reference.
const unsigned long kTimeout = UiState::kFocusTimeoutMs;
const int kMenuItems = UiState::kMenuItemCount;

UiContext ctx(bool leftStopSet, bool rightStopSet, bool motionEnabled = false,
              bool motionActive = false) {
  UiContext c;
  c.leftStopSet = leftStopSet;
  c.rightStopSet = rightStopSet;
  c.motionEnabled = motionEnabled;
  c.motionActive = motionActive;
  return c;
}

const UiContext kNoStops = ctx(false, false);
const UiContext kBothStops = ctx(true, true);
const UiContext kLeftOnly = ctx(true, false);
const UiContext kRightOnly = ctx(false, true);

// A UiState plus a virtual millisecond clock, so timeout tests are explicit
// about when each event happened.
class Rig {
 public:
  Rig() : m_now(1000) {}

  UiIntent key(UiKey k, UiKeyEvent ev, const UiContext& c = kNoStops) {
    return m_ui.handleKey(k, ev, c, m_now);
  }

  // Full short-press gesture; returns the intent produced by the Click.
  // Press and Release intents are discarded - use key() when they matter.
  UiIntent click(UiKey k, const UiContext& c = kNoStops) {
    m_ui.handleKey(k, UiKeyEvent::Press, c, m_now);
    UiIntent out = m_ui.handleKey(k, UiKeyEvent::Click, c, m_now);
    m_ui.handleKey(k, UiKeyEvent::Release, c, m_now);
    return out;
  }

  // Full long-press gesture; returns the intent produced by the Hold.
  UiIntent hold(UiKey k, const UiContext& c = kNoStops) {
    m_ui.handleKey(k, UiKeyEvent::Press, c, m_now);
    UiIntent out = m_ui.handleKey(k, UiKeyEvent::Hold, c, m_now);
    m_ui.handleKey(k, UiKeyEvent::Release, c, m_now);
    return out;
  }

  // Drive focus into a given state using only real key gestures.
  void enterFocus(UiFocus f) {
    switch (f) {
      case UiFocus::Jog: break;
      case UiFocus::JogSpeed: click(UiKey::Ok); break;
      case UiFocus::Rate: click(UiKey::Rate); break;
      case UiFocus::Mode: click(UiKey::Mode); break;
      case UiFocus::Stops: click(UiKey::Stops); break;
      case UiFocus::Menu: click(UiKey::Menu); break;
    }
  }

  bool tick() { return m_ui.tick(m_now); }
  void advance(unsigned long ms) { m_now += ms; }
  unsigned long now() const { return m_now; }

  UiState& ui() { return m_ui; }
  UiFocus focus() const { return m_ui.focus(); }

 private:
  UiState m_ui;
  unsigned long m_now;
};

const UiFocus kAllFocuses[] = {UiFocus::Jog,   UiFocus::JogSpeed, UiFocus::Rate,
                               UiFocus::Mode,  UiFocus::Stops,    UiFocus::Menu};

// ===========================================================================
// 1. Rest state
// ===========================================================================

TEST(UiStateRest, FocusStartsAtJog) {
  UiState ui;
  EXPECT_EQ(UiFocus::Jog, ui.focus());
}

TEST(UiStateRest, MenuStartsClosedAtIndexZero) {
  UiState ui;
  EXPECT_FALSE(ui.menuOpen());
  EXPECT_EQ(0, ui.menuIndex());
}

// ===========================================================================
// 2. Jog on the bare arrows, context-driven (spec §3)
// ===========================================================================

TEST(UiStateJog, LeftClickWithLeftStopSetRunsToLeftStop) {
  Rig r;
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateJog, RightClickWithRightStopSetRunsToRightStop) {
  Rig r;
  EXPECT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, kRightOnly));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateJog, LeftHoldWithLeftStopSetAlsoRunsToLeftStop) {
  // Spec §3 table: "Hold, left stop SET -> (same as click)". Safe because the
  // keypad emits no Click after a Hold, so this cannot double-fire.
  Rig r;
  EXPECT_EQ(UiIntent::RunToLeftStop, r.hold(UiKey::Left, kLeftOnly));
}

TEST(UiStateJog, RightHoldWithRightStopSetAlsoRunsToRightStop) {
  Rig r;
  EXPECT_EQ(UiIntent::RunToRightStop, r.hold(UiKey::Right, kRightOnly));
}

TEST(UiStateJog, LeftPressWithLeftStopUnsetStartsJogAndReleaseStops) {
  Rig r;
  EXPECT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateJog, RightPressWithRightStopUnsetStartsJogAndReleaseStops) {
  Rig r;
  EXPECT_EQ(UiIntent::JogRightStart, r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
}

TEST(UiStateJog, ClickIsInertWhenTheStopIsUnset) {
  // Decision: with no stop, the Press already started the jog and the Release
  // will stop it; the Click that arrives between them must do nothing, or a
  // short tap would emit a spurious second action.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Click, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Click, kNoStops));
}

TEST(UiStateJog, HoldIsInertWhenTheStopIsUnset) {
  // Decision: hold-to-jog is already running from the Press, so the Hold event
  // is redundant. "Continuous jog while held" is Press/Release, not Hold.
  Rig r;
  EXPECT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Hold, kNoStops));
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
}

TEST(UiStateJog, PressAndReleaseAreInertWhenTheStopIsSet) {
  // Decision: with a stop set the gesture is click-to-run, so the surrounding
  // Press/Release must not emit jog start/stop - a Release must not abort the
  // powered run that its own Click just started.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Press, kLeftOnly));
  EXPECT_EQ(UiIntent::RunToLeftStop, r.key(UiKey::Left, UiKeyEvent::Click, kLeftOnly));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kLeftOnly));
}

TEST(UiStateJog, SecondLeftClickDuringPoweredRunCancels) {
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, kLeftOnly));
}

TEST(UiStateJog, SecondRightClickDuringPoweredRunCancels) {
  Rig r;
  ASSERT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, kRightOnly));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Right, kRightOnly));
}

TEST(UiStateJog, ThirdClickAfterCancelStartsAFreshRun) {
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  ASSERT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
}

TEST(UiStateJog, OppositeArrowDuringPoweredRunCancels) {
  // Decision (spec §7: a powered run is "cancellable by any of three keys").
  // The opposite arrow cancels rather than immediately reversing - one gesture,
  // one effect, and the reverse run is then one more click away.
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kBothStops));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Right, kBothStops));
}

TEST(UiStateJog, ArrowsInhibitedWhileMotionEnabled) {
  // Spec §3: "Arrows are inhibited while MM_ENABLED." Every arrow, every event,
  // both stop states.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Click, UiKeyEvent::Hold};
  const bool stopStates[] = {false, true};
  for (UiKey k : arrows) {
    for (bool stopsSet : stopStates) {
      for (UiKeyEvent ev : events) {
        Rig r;
        UiContext c = ctx(stopsSet, stopsSet, /*motionEnabled=*/true);
        EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
            << "key=" << (k == UiKey::Left ? "Left" : "Right")
            << " event=" << ev << " stopsSet=" << stopsSet;
      }
    }
  }
}

TEST(UiStateJog, ReleaseAlwaysStopsAnInFlightJogEvenIfTheStopAppeared) {
  // Regression: a dead-man jog is only safe if letting go ALWAYS stops it.
  // If a stop becomes set between the Press and the Release, the Release must
  // still emit JogStop - otherwise the release falls into the click-to-run
  // branch (where Press/Release are inert), m_jogDir is stranded, and the
  // carriage keeps moving with no JogStop ever emitted.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, kLeftOnly));
  // And the jog is genuinely over: a fresh Press starts a new one.
  EXPECT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
}

TEST(UiStateJog, ReleaseAlwaysStopsAnInFlightJogEvenIfMotionBecameEnabled) {
  // Same hazard via the other early return: the §3 MM_ENABLED inhibit must not
  // swallow the Release that ends a jog already in flight.
  Rig r;
  ASSERT_EQ(UiIntent::JogRightStart, r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop,
            r.key(UiKey::Right, UiKeyEvent::Release, ctx(false, false, true)));
}

TEST(UiStateJog, ReleaseAlwaysStopsAnInFlightJogEvenFromAnotherFocus) {
  // Focus cannot normally change mid-jog (the matrix scans one key at a time),
  // but if it did, the widget branch would swallow the Release. The terminator
  // sits above every focus test precisely so that cannot happen.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  r.click(UiKey::Rate);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiFocus::Rate, r.focus()) << "stopping the jog must not steal focus";
}

TEST(UiStateJog, ReleaseAlwaysStopsAnInFlightJogEvenWithTheMenuOpen) {
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
  EXPECT_TRUE(r.ui().menuOpen()) << "stopping the jog must not close the menu";
}

TEST(UiStateJog, OppositeArrowReleaseAlsoStopsAnInFlightJog) {
  // Decision: the terminator is NOT direction-matched. A stray release of the
  // other arrow should not be able to leave the carriage running; erring
  // towards "stop" costs a shortened jog, erring the other way costs a crash.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops))
      << "the jog is already stopped; a second release must not re-fire";
}

TEST(UiStateJog, ArrowReleaseWithNoJogInFlightIsInert) {
  // The other direction of the same contract: the terminator must fire ONLY
  // when a jog is actually running, or it would emit JogStop on the release of
  // every click-to-run tap and abort the run that tap just started.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Release, kBothStops));
  ASSERT_EQ(UiIntent::RunToLeftStop, r.key(UiKey::Left, UiKeyEvent::Click, kLeftOnly));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kLeftOnly));
}

TEST(UiStateJog, HaltStillOutranksTheJogTerminator) {
  // HALT is above the terminator in the ladder, so a HALT release stays inert
  // and a HALT during a jog still reports CancelMotion, not JogStop.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops))
      << "HALT already cleared the jog; the release must not re-fire";
}

TEST(UiStateJog, ArrowsDoNotChangeFocus) {
  Rig r;
  r.click(UiKey::Left, kLeftOnly);
  EXPECT_EQ(UiFocus::Jog, r.focus());
  r.key(UiKey::Right, UiKeyEvent::Press, kNoStops);
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateJog, ReleaseAlwaysStopsAnInFlightJogEvenIfMotionBecameActive) {
  // The new ctx.motionActive field (D2 fix) must not open a new hole in the
  // dead-man terminator, the same way ctx.motionEnabled did before D2's own
  // review pass (see ReleaseAlwaysStopsAnInFlightJogEvenIfMotionBecameEnabled
  // above).
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop,
            r.key(UiKey::Left, UiKeyEvent::Release,
                  ctx(false, false, /*motionEnabled=*/false, /*motionActive=*/true)));
}

// ---------------------------------------------------------------------------
// Defect 2 (FS-D2 review, HIGH): m_runToStopDir is UiState's own duplicate of
// "a powered run is in flight", set on the click that starts a run and (pre-
// D2) cleared only by an arrow gesture or HALT. But a run also ends BY ITSELF
// when the carriage reaches the stop, and nothing told UiState. So the first
// arrow click after any naturally-completed run used to return CancelMotion
// instead of starting the next run, and the click was silently eaten - run
// left to the stop, click ▶ to come back, nothing happens, click again.
//
// The fix reconciles the latch against ctx.motionActive: whenever the caller
// reports motionActive false, the latch is cleared, so a naturally-completed
// run leaves no stale state and the very next arrow click starts a fresh run.
// ---------------------------------------------------------------------------

TEST(UiStateJog, NaturallyCompletedLeftRunStartsAFreshRunOnTheNextClick) {
  Rig r;
  // The run starts, and motionActive is (or becomes) true while it is
  // genuinely in flight.
  UiContext running = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, running));

  // The carriage reaches the stop on its own - no key event from the
  // operator - and the next context the display hands to UiState reflects
  // that motion is no longer active.
  UiContext idle = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/false);

  // The very next arrow click must start a fresh run, NOT cancel a stale one.
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

TEST(UiStateJog, NaturallyCompletedRightRunStartsAFreshRunOnTheNextClick) {
  Rig r;
  UiContext running = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, running));
  UiContext idle = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/false);
  EXPECT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, idle));
}

TEST(UiStateJog, InFlightRunStillCancelsOnTheNextClick) {
  // Control for the reconciliation fix above: while motionActive genuinely
  // stays true (the run has not completed), the existing cancel-on-second-
  // click behaviour (spec §7) must be unchanged - the reconciliation must not
  // make every powered run uncancellable.
  Rig r;
  UiContext running = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, running));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, running));
}

TEST(UiStateJog, InFlightRightRunStillCancelsOnTheNextClick) {
  Rig r;
  UiContext running = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, running));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Right, running));
}

// ===========================================================================
// 3. The OK key's three jobs (spec §1 table)
// ===========================================================================

TEST(UiStateOk, ClickAtRestOpensJogSpeed) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok));
  EXPECT_EQ(UiFocus::JogSpeed, r.focus());
}

TEST(UiStateOk, ClickWithWidgetOpenCommitsAndReturnsToJog) {
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops};
  for (UiFocus f : widgets) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok)) << "from " << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "from " << f;
  }
}

TEST(UiStateOk, HoldAtRestZerosTheDroAndKeepsJogFocus) {
  Rig r;
  EXPECT_EQ(UiIntent::ZeroDro, r.hold(UiKey::Ok));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateOk, HoldWithWidgetOpenDoesNotZeroTheDro) {
  // Spec §1: the Hold gesture is defined only "at rest". Decision: elsewhere it
  // is inert and leaves focus alone, so a slow OK press inside a widget cannot
  // silently move the datum.
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops};
  for (UiFocus f : widgets) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::None, r.hold(UiKey::Ok)) << "from " << f;
    EXPECT_EQ(f, r.focus()) << "from " << f;
  }
}

TEST(UiStateOk, PressAndReleaseAreInert) {
  // Decision: OK acts on Click / Hold only, so the surrounding Press and
  // Release of the same gesture cannot fire a second time.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Press));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Release));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateOk, ZeroDroStillWorksWhileMotionEnabled) {
  // Decision: zeroing the DRO moves no metal, so the MM_ENABLED arrow inhibit
  // (spec §3) does not extend to it.
  Rig r;
  EXPECT_EQ(UiIntent::ZeroDro, r.hold(UiKey::Ok, ctx(false, false, true)));
}

// ===========================================================================
// 4. Selectors move focus and the arrows follow (spec §1, §4)
// ===========================================================================

TEST(UiStateSelectors, ModeKeyTakesFocusAndArrowsStepTheMode) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Mode));
  ASSERT_EQ(UiFocus::Mode, r.focus());
  EXPECT_EQ(UiIntent::ModePrev, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::ModeNext, r.click(UiKey::Right));
  EXPECT_EQ(UiFocus::Mode, r.focus());
}

TEST(UiStateSelectors, RateKeyTakesFocusAndArrowsStepThePitch) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Rate));
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::PitchPrev, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::PitchNext, r.click(UiKey::Right));
  EXPECT_EQ(UiFocus::Rate, r.focus());
}

TEST(UiStateSelectors, JogSpeedFocusArrowsStepTheJogSpeed) {
  Rig r;
  r.click(UiKey::Ok);
  ASSERT_EQ(UiFocus::JogSpeed, r.focus());
  EXPECT_EQ(UiIntent::JogSpeedPrev, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::JogSpeedNext, r.click(UiKey::Right));
  EXPECT_EQ(UiFocus::JogSpeed, r.focus());
}

TEST(UiStateSelectors, StopsKeyTakesFocus) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateSelectors, ArrowsIgnorePressAndReleaseInsideAWidget) {
  // Decision - the biggest gap in the spec. Selector widgets step on discrete
  // Clicks (and, for STOPS, Holds); the Press/Release pair of the same gesture
  // must be inert, or every tap would step twice. It also means the dead-man
  // jog cannot be started from inside a widget, matching §3: "while the
  // jog-speed widget is open the arrows own the speed, not the carriage".
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops, UiFocus::Menu};
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiFocus f : widgets) {
    for (UiKey k : arrows) {
      Rig r;
      r.enterFocus(f);
      EXPECT_EQ(UiIntent::None, r.key(k, UiKeyEvent::Press, kNoStops))
          << "focus=" << f;
      EXPECT_EQ(UiIntent::None, r.key(k, UiKeyEvent::Release, kNoStops))
          << "focus=" << f;
      EXPECT_EQ(f, r.focus());
    }
  }
}

TEST(UiStateSelectors, ModeAndRateIgnoreHold) {
  // Decision: only STOPS gives Hold a meaning (§4). Elsewhere it is inert.
  Rig r;
  r.click(UiKey::Mode);
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Left));
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Right));
  Rig r2;
  r2.click(UiKey::Rate);
  EXPECT_EQ(UiIntent::None, r2.hold(UiKey::Left));
  EXPECT_EQ(UiIntent::None, r2.hold(UiKey::Right));
}

TEST(UiStateSelectors, AnotherSelectorMovesFocusDirectly) {
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  r.click(UiKey::Rate);
  EXPECT_EQ(UiFocus::Rate, r.focus());
  r.click(UiKey::Stops);
  EXPECT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetLeftStop, r.click(UiKey::Left, kNoStops));
}

TEST(UiStateSelectors, RepeatingTheSameSelectorKeepsItsFocus) {
  // Decision: §1 lists the leave conditions for a widget as OK / HALT / 4 s
  // idle - the selector key itself is not one of them, so a second press is a
  // no-op that simply restarts the idle timer. (MENU is the exception, §6.)
  Rig r;
  r.click(UiKey::Mode);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Mode));
  EXPECT_EQ(UiFocus::Mode, r.focus());
}

TEST(UiStateSelectors, SelectorsStillWorkWhileMotionEnabled) {
  // Decision: the §3 inhibit is about the carriage, not the widgets - changing
  // pitch or mode mid-cut must stay possible (and is instantly reversible).
  Rig r;
  UiContext c = ctx(false, false, true);
  r.click(UiKey::Rate, c);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::PitchNext, r.click(UiKey::Right, c));
}

// ===========================================================================
// 5. STOPS asymmetry (spec §4 - the safety-relevant one)
// ===========================================================================

TEST(UiStateStops, LeftClickWithLeftStopUnsetSetsIt) {
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetLeftStop, r.click(UiKey::Left, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, LeftClickWithLeftStopSetDoesNothing) {
  // "Flash 'hold to clear' - no action."
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, kLeftOnly));
}

TEST(UiStateStops, LeftHoldWithLeftStopSetClearsIt) {
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::ClearLeftStop, r.hold(UiKey::Left, kLeftOnly));
}

TEST(UiStateStops, LeftHoldWithLeftStopUnsetDoesNothing) {
  // Decision: hold means "clear"; with nothing to clear it is inert rather
  // than falling back to set (a hold must never set a stop by accident).
  Rig r;
  r.click(UiKey::Stops);
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Left, kNoStops));
}

TEST(UiStateStops, RightClickWithRightStopUnsetSetsIt) {
  Rig r;
  r.click(UiKey::Stops);
  EXPECT_EQ(UiIntent::SetRightStop, r.click(UiKey::Right, kNoStops));
}

TEST(UiStateStops, RightClickWithRightStopSetDoesNothing) {
  Rig r;
  r.click(UiKey::Stops, kRightOnly);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, kRightOnly));
}

TEST(UiStateStops, RightHoldWithRightStopSetClearsIt) {
  Rig r;
  r.click(UiKey::Stops, kRightOnly);
  EXPECT_EQ(UiIntent::ClearRightStop, r.hold(UiKey::Right, kRightOnly));
}

TEST(UiStateStops, RightHoldWithRightStopUnsetDoesNothing) {
  Rig r;
  r.click(UiKey::Stops);
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Right, kNoStops));
}

TEST(UiStateStops, EachArrowOnlySeesItsOwnStop) {
  // Left stop set, right unset: left click is inert, right click sets.
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiIntent::SetRightStop, r.click(UiKey::Right, kLeftOnly));
}

// ---------------------------------------------------------------------------
// No stop edits while the leadscrew is engaged.
//
// Decision (review of FS-D): the §3 MM_ENABLED inhibit covers STOPS as well as
// JOG. One rule, both arrows, both directions - you must disengage to change a
// stop.
//
// Clearing is the dangerous half: `hitLeftEndstop()` is the ONLY thing that
// arrests an MM_ENABLED feed (leadscrew.cpp:239-240) and it is false whenever
// the stop is UNSET, so clearing the stop you are cutting towards deletes the
// arrest and the carriage feeds into the chuck. Clearing the far stop instead
// re-anchors the helix through LeadscrewStopSync::unsetStop, shifting the
// thread phase mid-cut - or losing the anchor entirely if no stop survives.
//
// Setting is unsafe differently: setting the stop you are feeding into slams
// the feed to MM_DISABLED mid-thread, setting the far one silently latches the
// spindle sync anchor, and neither can capture the position the operator
// actually saw, because DisplayTask sleeps 100 ms between polls.
// ---------------------------------------------------------------------------

TEST(UiStateStops, ClickDoesNotSetAStopWhileMotionEnabled) {
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    Rig r;
    UiContext c = ctx(false, false, /*motionEnabled=*/true);
    r.click(UiKey::Stops, c);
    ASSERT_EQ(UiFocus::Stops, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(k, c))
        << "arrow=" << (k == UiKey::Left ? "Left" : "Right");
  }
}

TEST(UiStateStops, HoldDoesNotClearAStopWhileMotionEnabled) {
  // The one that would run the carriage into the chuck.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    Rig r;
    UiContext c = ctx(true, true, /*motionEnabled=*/true);
    r.click(UiKey::Stops, c);
    ASSERT_EQ(UiFocus::Stops, r.focus());
    EXPECT_EQ(UiIntent::None, r.hold(k, c))
        << "arrow=" << (k == UiKey::Left ? "Left" : "Right");
  }
}

TEST(UiStateStops, NoStopEditIsPossibleWhileMotionEnabled) {
  // Exhaustive: every arrow, every event, every stop state, engaged.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Click, UiKeyEvent::Hold};
  const bool stopStates[] = {false, true};
  for (UiKey k : arrows) {
    for (bool ls : stopStates) {
      for (bool rs : stopStates) {
        for (UiKeyEvent ev : events) {
          Rig r;
          UiContext c = ctx(ls, rs, /*motionEnabled=*/true);
          r.click(UiKey::Stops, c);
          ASSERT_EQ(UiFocus::Stops, r.focus());
          EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
              << "arrow=" << (k == UiKey::Left ? "Left" : "Right")
              << " event=" << ev << " left=" << ls << " right=" << rs;
        }
      }
    }
  }
}

TEST(UiStateStops, TheInhibitLiftsWhenDisengaged) {
  // The other direction of the contract: the inhibit is about MM_ENABLED and
  // nothing else, so the identical gestures work once disengaged. Without this
  // the tests above would pass on a state machine that had simply broken STOPS.
  const UiContext engaged = ctx(false, false, true);
  const UiContext engagedSet = ctx(true, true, true);

  Rig setL;
  setL.click(UiKey::Stops, engaged);
  ASSERT_EQ(UiIntent::None, setL.click(UiKey::Left, engaged));
  EXPECT_EQ(UiIntent::SetLeftStop, setL.click(UiKey::Left, kNoStops));

  Rig setR;
  setR.click(UiKey::Stops, engaged);
  ASSERT_EQ(UiIntent::None, setR.click(UiKey::Right, engaged));
  EXPECT_EQ(UiIntent::SetRightStop, setR.click(UiKey::Right, kNoStops));

  Rig clearL;
  clearL.click(UiKey::Stops, engagedSet);
  ASSERT_EQ(UiIntent::None, clearL.hold(UiKey::Left, engagedSet));
  EXPECT_EQ(UiIntent::ClearLeftStop, clearL.hold(UiKey::Left, kBothStops));

  Rig clearR;
  clearR.click(UiKey::Stops, engagedSet);
  ASSERT_EQ(UiIntent::None, clearR.hold(UiKey::Right, engagedSet));
  EXPECT_EQ(UiIntent::ClearRightStop, clearR.hold(UiKey::Right, kBothStops));
}

TEST(UiStateStops, TheStopsWidgetStillOpensWhileMotionEnabled) {
  // Only the edits are inhibited, not the view: the travel bar with both stops
  // and the live carriage position (§4) is exactly what you want on screen
  // mid-cut. The widget must still take focus and still time out normally.
  Rig r;
  UiContext c = ctx(true, true, /*motionEnabled=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops, c));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  r.advance(kTimeout);
  EXPECT_TRUE(r.tick());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

// ---------------------------------------------------------------------------
// Defect 1 (FS-D2 review, CRITICAL): ctx.motionEnabled means MM_ENABLED only.
// A powered run-to-stop is MM_JOG_LEFT/MM_JOG_RIGHT, so motionEnabled is FALSE
// for the whole run - the block above does not cover it. Three ordinary
// keypresses (click an arrow to start a run-to-stop, press STOPS, hold the
// same arrow) would otherwise clear the stop the carriage is travelling
// towards while it is still under power; hitLeftEndstop() is the SOLE arrest
// for that run (leadscrew.cpp:233) and unsetStop sets the position to
// INT32_MIN, so the run would never terminate. ctx.motionActive is the
// broader "something is moving under power right now" signal (engaged feed,
// powered run-to-stop, interactive jog, deceleration) and must gate stop
// edits the same way motionEnabled does.
// ---------------------------------------------------------------------------

TEST(UiStateStops, ClickDoesNotSetAStopWhileMotionActive) {
  // The powered-run case: motionEnabled is false (it is a JOG mode, not
  // ENABLED) but motionActive is true. This is the crash bug - without the
  // gate, this click clears/sets a stop while the carriage runs toward it.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    Rig r;
    UiContext c = ctx(false, false, /*motionEnabled=*/false, /*motionActive=*/true);
    r.click(UiKey::Stops, c);
    ASSERT_EQ(UiFocus::Stops, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(k, c))
        << "arrow=" << (k == UiKey::Left ? "Left" : "Right");
  }
}

TEST(UiStateStops, HoldDoesNotClearAStopWhileMotionActive) {
  // The dangerous half, during a powered run: clearing the stop the carriage
  // is travelling towards deletes the only thing that arrests it.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    Rig r;
    UiContext c = ctx(true, true, /*motionEnabled=*/false, /*motionActive=*/true);
    r.click(UiKey::Stops, c);
    ASSERT_EQ(UiFocus::Stops, r.focus());
    EXPECT_EQ(UiIntent::None, r.hold(k, c))
        << "arrow=" << (k == UiKey::Left ? "Left" : "Right");
  }
}

TEST(UiStateStops, NoStopEditIsPossibleWhileMotionActive) {
  // Exhaustive: every arrow, every event, every stop state, motionActive true
  // and motionEnabled explicitly false throughout - this is the case the
  // pre-D2 gate (`if (ctx.motionEnabled)`) completely misses.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Click, UiKeyEvent::Hold};
  const bool stopStates[] = {false, true};
  for (UiKey k : arrows) {
    for (bool ls : stopStates) {
      for (bool rs : stopStates) {
        for (UiKeyEvent ev : events) {
          Rig r;
          UiContext c = ctx(ls, rs, /*motionEnabled=*/false, /*motionActive=*/true);
          r.click(UiKey::Stops, c);
          ASSERT_EQ(UiFocus::Stops, r.focus());
          EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
              << "arrow=" << (k == UiKey::Left ? "Left" : "Right")
              << " event=" << ev << " left=" << ls << " right=" << rs;
        }
      }
    }
  }
}

TEST(UiStateStops, TheMotionActiveInhibitLiftsWhenBothFlagsAreFalse) {
  // The control: with motionEnabled AND motionActive both false, the exact
  // same gestures that were just proven inert above must still work. Without
  // this, the tests above would pass on a state machine that had simply
  // broken STOPS outright rather than correctly gating it.
  const UiContext atRest = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/false);
  const UiContext atRestSet = ctx(true, true, /*motionEnabled=*/false,
                                  /*motionActive=*/false);

  Rig setL;
  setL.click(UiKey::Stops, atRest);
  EXPECT_EQ(UiIntent::SetLeftStop, setL.click(UiKey::Left, atRest));

  Rig setR;
  setR.click(UiKey::Stops, atRest);
  EXPECT_EQ(UiIntent::SetRightStop, setR.click(UiKey::Right, atRest));

  Rig clearL;
  clearL.click(UiKey::Stops, atRestSet);
  EXPECT_EQ(UiIntent::ClearLeftStop, clearL.hold(UiKey::Left, atRestSet));

  Rig clearR;
  clearR.click(UiKey::Stops, atRestSet);
  EXPECT_EQ(UiIntent::ClearRightStop, clearR.hold(UiKey::Right, atRestSet));
}

TEST(UiStateStops, TheStopsWidgetStillOpensWhileMotionActive) {
  // As with motionEnabled, only the edits are inhibited, not the view: the
  // travel bar is exactly what you want on screen during a powered run.
  Rig r;
  UiContext c = ctx(true, true, /*motionEnabled=*/false, /*motionActive=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops, c));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  r.advance(kTimeout);
  EXPECT_TRUE(r.tick());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

// ===========================================================================
// 6. HALT is always live and outranks everything (spec §5)
// ===========================================================================

TEST(UiStateHalt, ClickFromEveryFocusCancelsAndReturnsToJog) {
  for (UiFocus f : kAllFocuses) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus()) << "setup failed for " << f;
    EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt, kBothStops))
        << "from " << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "from " << f;
    EXPECT_FALSE(r.ui().menuOpen()) << "from " << f;
  }
}

TEST(UiStateHalt, ClosesAnOpenMenu) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateHalt, CancelsMidJog) {
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateHalt, CancelsMidPoweredRunAndClearsTheRunFlag) {
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  ASSERT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt, kLeftOnly));
  // The next arrow click must start a fresh run, not cancel a stale one.
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
}

TEST(UiStateHalt, WorksWhileMotionEnabled) {
  // The arrow inhibit must not extend to HALT - that would be the exact
  // opposite of what §5 asks for.
  Rig r;
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt, ctx(true, true, true)));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateHalt, ActsOnPressAndOnHoldToo) {
  // Decision: HALT is safety-critical, so it fires on the earliest event of the
  // gesture as well as the Click, and on a Hold. CancelMotion is idempotent, so
  // a short press cancelling twice (Press then Click) is harmless.
  Rig r;
  r.enterFocus(UiFocus::Mode);
  EXPECT_EQ(UiIntent::CancelMotion, r.key(UiKey::Halt, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());

  Rig r2;
  r2.enterFocus(UiFocus::Menu);
  EXPECT_EQ(UiIntent::CancelMotion, r2.hold(UiKey::Halt, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r2.focus());
  EXPECT_FALSE(r2.ui().menuOpen());
}

TEST(UiStateHalt, ReleaseIsInert) {
  // Decision: the Release that ends a HALT gesture must not re-fire.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Halt, UiKeyEvent::Release, kNoStops));
}

// ===========================================================================
// 7. Focus timeout (spec §1: "4 s idle")
// ===========================================================================

TEST(UiStateTimeout, TickIsFalseWhenNothingChanges) {
  Rig r;
  EXPECT_FALSE(r.tick());
  r.advance(kTimeout * 3);
  EXPECT_FALSE(r.tick()) << "resting at Jog must never report a change";
}

TEST(UiStateTimeout, FocusFallsBackToJogAfterTheTimeout) {
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick());
  EXPECT_EQ(UiFocus::Mode, r.focus());
  r.advance(1);  // now exactly kFocusTimeoutMs since the key event
  EXPECT_TRUE(r.tick()) << "timeout fires at exactly kFocusTimeoutMs";
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateTimeout, TickReturnsTrueOnlyOnce) {
  Rig r;
  r.click(UiKey::Rate);
  r.advance(kTimeout);
  ASSERT_TRUE(r.tick());
  EXPECT_FALSE(r.tick()) << "a redraw trigger must not repeat";
  r.advance(kTimeout * 2);
  EXPECT_FALSE(r.tick());
}

TEST(UiStateTimeout, AKeyEventResetsTheTimeout) {
  Rig r;
  r.click(UiKey::Rate);
  r.advance(kTimeout - 100);
  EXPECT_FALSE(r.tick());
  r.click(UiKey::Right);  // activity
  r.advance(kTimeout - 100);
  EXPECT_FALSE(r.tick()) << "the arrow press should have restarted the clock";
  EXPECT_EQ(UiFocus::Rate, r.focus());
  r.advance(100);
  EXPECT_TRUE(r.tick());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateTimeout, AppliesToEveryWidgetFocus) {
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops};
  for (UiFocus f : widgets) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    r.advance(kTimeout);
    EXPECT_TRUE(r.tick()) << "from " << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "from " << f;
  }
}

TEST(UiStateTimeout, MenuDoesNotTimeOut) {
  // Spec §1: MENU leaves on MENU or HALT only.
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_EQ(UiFocus::Menu, r.focus());
  r.advance(kTimeout * 10);
  EXPECT_FALSE(r.tick());
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
}

TEST(UiStateTimeout, PoweredRunIsNotCancelledByTheTimeout) {
  // The idle timeout only moves focus; it must never emit motion changes, and
  // the pending run must survive so the next arrow click still cancels it.
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  r.advance(kTimeout * 2);
  EXPECT_FALSE(r.tick()) << "focus was already Jog, so nothing changed";
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, kLeftOnly));
}

TEST(UiStateTimeout, AnInertKeyEventAlsoResetsTheTimeout) {
  // The header says "Any key event resets the focus idle timeout" - including
  // events that produce no intent. Only AKeyEventResetsTheTimeout's acting
  // Click was pinned; this covers the inert half.
  Rig r;
  r.click(UiKey::Mode);
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick());
  // Press is inert inside a widget (it produces no intent) but is still
  // activity.
  ASSERT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Press));
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick()) << "an inert event must still restart the clock";
  EXPECT_EQ(UiFocus::Mode, r.focus());
  r.advance(1);
  EXPECT_TRUE(r.tick());
}

TEST(UiStateTimeout, SurvivesMillisRollover) {
  // nowMs is millis(), which wraps at ~49.7 days. The comparison must be the
  // unsigned-difference form; `m_lastActivityMs + kFocusTimeoutMs <= nowMs`
  // would overflow across the wrap and either fire ~49 days early or never.
  const unsigned long kNearMax = ~0UL - (kTimeout / 2);
  UiState ui;
  UiContext c = ctx(false, false);

  // Key event just before the wrap...
  ASSERT_EQ(UiIntent::None, ui.handleKey(UiKey::Mode, UiKeyEvent::Press, c, kNearMax));
  ASSERT_EQ(UiIntent::None, ui.handleKey(UiKey::Mode, UiKeyEvent::Click, c, kNearMax));
  ASSERT_EQ(UiFocus::Mode, ui.focus());

  // ...still inside the window one tick before the deadline, having wrapped.
  const unsigned long justBefore = kNearMax + kTimeout - 1;  // wraps
  ASSERT_LT(justBefore, kNearMax) << "the test clock must actually have wrapped";
  EXPECT_FALSE(ui.tick(justBefore));
  EXPECT_EQ(UiFocus::Mode, ui.focus());

  // ...and fires at exactly kFocusTimeoutMs across the wrap.
  EXPECT_TRUE(ui.tick(kNearMax + kTimeout));
  EXPECT_EQ(UiFocus::Jog, ui.focus());
}

// ===========================================================================
// 8. MENU (spec §6)
// ===========================================================================

TEST(UiStateMenu, MenuKeyOpensAtIndexZero) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu));
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateMenu, ArrowsMoveThroughTheTiles) {
  Rig r;
  r.click(UiKey::Menu);
  EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right));
  EXPECT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right));
  EXPECT_EQ(2, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuPrev, r.click(UiKey::Left));
  EXPECT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiFocus::Menu, r.focus());
}

TEST(UiStateMenu, IndexClampsAtTheStart) {
  // Clamping contract: the index saturates, it does NOT wrap, and a blocked
  // move reports UiIntent::None so the display has no reason to redraw.
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_EQ(0, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left));
  EXPECT_EQ(0, r.ui().menuIndex());
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
}

TEST(UiStateMenu, IndexClampsAtTheEnd) {
  Rig r;
  r.click(UiKey::Menu);
  for (int i = 1; i < kMenuItems; ++i) {
    EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right)) << "step " << i;
    EXPECT_EQ(i, r.ui().menuIndex());
  }
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Right));
  EXPECT_EQ(kMenuItems - 1, r.ui().menuIndex());
  // Extra pushes must not run away.
  r.click(UiKey::Right);
  r.click(UiKey::Right);
  EXPECT_EQ(kMenuItems - 1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuPrev, r.click(UiKey::Left));
  EXPECT_EQ(kMenuItems - 2, r.ui().menuIndex());
}

TEST(UiStateMenu, OkActivatesTheCurrentTileAndLeavesTheMenuOpen) {
  // Decision: tiles like Units / Theme toggle in place, so activation must not
  // dismiss the carousel; MENU or HALT close it (§6).
  Rig r;
  r.click(UiKey::Menu);
  r.click(UiKey::Right);
  ASSERT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuActivate, r.click(UiKey::Ok));
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(1, r.ui().menuIndex()) << "activation must not move the selection";
}

TEST(UiStateMenu, OkHoldInsideTheMenuDoesNotZeroTheDro) {
  Rig r;
  r.click(UiKey::Menu);
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Ok));
  EXPECT_EQ(UiFocus::Menu, r.focus());
}

TEST(UiStateMenu, MenuKeyAgainCloses) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::CloseMenu, r.click(UiKey::Menu));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenu, ReopeningStartsAtIndexZero) {
  // Decision: the carousel is not a resumable list; every open starts at Units.
  Rig r;
  r.click(UiKey::Menu);
  r.click(UiKey::Right);
  r.click(UiKey::Right);
  ASSERT_EQ(2, r.ui().menuIndex());
  r.click(UiKey::Menu);
  r.click(UiKey::Menu);
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateMenu, MenuKeyOpensOverAnyWidget) {
  // Decision: MENU is top level, so it replaces an open selector widget rather
  // than being swallowed by it.
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops};
  for (UiFocus f : widgets) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu)) << "from " << f;
    EXPECT_EQ(UiFocus::Menu, r.focus()) << "from " << f;
    EXPECT_TRUE(r.ui().menuOpen()) << "from " << f;
  }
}

TEST(UiStateMenu, SelectorKeysAreIgnoredWhileTheMenuIsOpen) {
  // Decision: §1 says the menu leaves only on MENU or HALT, so MODE / RATE /
  // STOPS must not steal focus out from under it.
  const UiKey selectors[] = {UiKey::Mode, UiKey::Rate, UiKey::Stops};
  for (UiKey k : selectors) {
    Rig r;
    r.click(UiKey::Menu);
    EXPECT_EQ(UiIntent::None, r.click(k, kBothStops));
    EXPECT_EQ(UiFocus::Menu, r.focus());
    EXPECT_TRUE(r.ui().menuOpen());
  }
}

TEST(UiStateMenu, ArrowsDoNotJogWhileTheMenuIsOpen) {
  Rig r;
  r.click(UiKey::Menu);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Press, kLeftOnly));
  EXPECT_EQ(UiFocus::Menu, r.focus());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
