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
    case UiFocus::DroDatum: return os << "Focus::DroDatum";
    case UiFocus::Menu: return os << "Focus::Menu";
    case UiFocus::Diagnostics: return os << "Focus::Diagnostics";
    case UiFocus::About: return os << "Focus::About";
    case UiFocus::Alarm: return os << "Focus::Alarm";
    case UiFocus::Ota: return os << "Focus::Ota";
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
    case UiIntent::ClearBothStops: return os << "ClearBothStops";
    case UiIntent::ToggleEngage: return os << "ToggleEngage";
    case UiIntent::ClearAlarm: return os << "ClearAlarm";
    case UiIntent::AckOta: return os << "AckOta";
    case UiIntent::ZeroDro: return os << "ZeroDro";
    case UiIntent::DroDatumLeft: return os << "DroDatumLeft";
    case UiIntent::DroDatumRight: return os << "DroDatumRight";
    case UiIntent::MenuNext: return os << "MenuNext";
    case UiIntent::MenuPrev: return os << "MenuPrev";
    case UiIntent::MenuActivate: return os << "MenuActivate";
    case UiIntent::CloseMenu: return os << "CloseMenu";
  }
  return os << "Intent::<?>";
}

std::ostream& operator<<(std::ostream& os, UiKey k) {
  switch (k) {
    case UiKey::Mode: return os << "Key::Mode";
    case UiKey::Rate: return os << "Key::Rate";
    case UiKey::Stops: return os << "Key::Stops";
    case UiKey::Left: return os << "Key::Left";
    case UiKey::Ok: return os << "Key::Ok";
    case UiKey::Right: return os << "Key::Right";
    case UiKey::Halt: return os << "Key::Halt";
    case UiKey::Menu: return os << "Key::Menu";
    case UiKey::Enable: return os << "Key::Enable";
    case UiKey::EncoderCw: return os << "Key::EncoderCw";
    case UiKey::EncoderCcw: return os << "Key::EncoderCcw";
  }
  return os << "Key::<?>";
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

// threadMode defaults to FALSE, i.e. a feed mode. Only menuTileBlock() reads
// it, and only for the Sync tile, so every test that is not about Sync is
// unaffected by the default; the ones that are pass it explicitly.
UiContext ctx(bool leftStopSet, bool rightStopSet, bool motionEnabled = false,
              bool motionActive = false, bool threadMode = false,
              bool alarm = false, bool ota = false) {
  UiContext c;
  c.leftStopSet = leftStopSet;
  c.rightStopSet = rightStopSet;
  c.motionEnabled = motionEnabled;
  c.motionActive = motionActive;
  c.threadMode = threadMode;
  c.alarm = alarm;
  c.ota = ota;
  return c;
}

const UiContext kNoStops = ctx(false, false);
const UiContext kBothStops = ctx(true, true);
const UiContext kLeftOnly = ctx(true, false);
const UiContext kRightOnly = ctx(false, true);
// At rest, in a thread mode: the context in which EVERY menu tile is available,
// so a test about a tile's destination is never accidentally testing a refusal.
const UiContext kThreadIdle = ctx(false, false, /*motionEnabled=*/false,
                                  /*motionActive=*/false, /*threadMode=*/true);

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

  // A tap that STARTS a powered run, with the context changing under it exactly
  // as it does on the machine.
  //
  // Since the motion lockout an arrow cannot start anything while the carriage
  // is already moving, so `before` must be an at-rest context - which is the
  // truth at the moment of the press, since the run has not begun. `after` is
  // the machine as the Release finds it: moving. ButtonPad rebuilds the context
  // from GlobalState after executing each intent, so this is the real sequence,
  // and it is also what promotes the run latch from Commanded to Confirmed.
  UiIntent startRun(UiKey k, const UiContext& before, const UiContext& after) {
    m_ui.handleKey(k, UiKeyEvent::Press, before, m_now);
    UiIntent out = m_ui.handleKey(k, UiKeyEvent::Click, before, m_now);
    m_ui.handleKey(k, UiKeyEvent::Release, after, m_now);
    return out;
  }

  // Full long-press gesture; returns the intent produced by the Hold.
  UiIntent hold(UiKey k, const UiContext& c = kNoStops) {
    m_ui.handleKey(k, UiKeyEvent::Press, c, m_now);
    UiIntent out = m_ui.handleKey(k, UiKeyEvent::Hold, c, m_now);
    m_ui.handleKey(k, UiKeyEvent::Release, c, m_now);
    return out;
  }

  // Open the carousel from rest, walk to `tile`, and press OK. Real gestures
  // only, and the context defaults to kThreadIdle so no tile is refused - a
  // helper that silently landed on a refusal would make every test built on it
  // pass for the wrong reason.
  UiIntent activateTile(int tile, const UiContext& c = kThreadIdle) {
    click(UiKey::Menu, c);
    for (int i = 0; i < tile; i++) {
      click(UiKey::Right, c);
    }
    return click(UiKey::Ok, c);
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
      // The three focuses that are only reachable through the menu.
      case UiFocus::DroDatum: activateTile(MENU_DRO_DATUM); break;
      case UiFocus::Diagnostics: activateTile(MENU_DIAGNOSTICS); break;
      case UiFocus::About: activateTile(MENU_ABOUT); break;
      // NOT reachable by any gesture, by design: the stepper-alarm modal is
      // forced by the machine, never chosen. Its tests drive it with a context
      // instead, which is the only way it can arise on the device either.
      case UiFocus::Alarm: break;
      // Same story for the OTA screen: GlobalState::hasOTA() forces it, no key
      // asks for it directly. See the UiStateOta section.
      case UiFocus::Ota: break;
    }
  }

  // Defaults to an AT-REST context, so the many idle-timeout tests read as they
  // always did. Close-on-motion tests pass a powered one explicitly.
  bool tick(const UiContext& c = kNoStops) { return m_ui.tick(c, m_now); }
  void advance(unsigned long ms) { m_now += ms; }
  unsigned long now() const { return m_now; }

  UiState& ui() { return m_ui; }
  UiFocus focus() const { return m_ui.focus(); }

 private:
  UiState m_ui;
  unsigned long m_now;
};

// The six focuses that are reachable from the rest screen with a single key.
//
// DELIBERATELY does NOT include DroDatum, Diagnostics or About. Those three are
// reachable only through a menu tile, and two of them answer several of the
// keys below differently on purpose - MENU closes a read-only screen instead of
// opening the carousel over it, and the encoder is inert there rather than
// stepping a value. Folding them in would make the "every focus" loops assert a
// uniformity that is not the design; they get their own tests instead, in the
// DroDatum / read-only-screen sections at the end of this file.
const UiFocus kAllFocuses[] = {UiFocus::Jog,   UiFocus::JogSpeed, UiFocus::Rate,
                               UiFocus::Mode,  UiFocus::Stops,    UiFocus::Menu};

// The three focuses a menu tile opens.
const UiFocus kMenuOpenedFocuses[] = {UiFocus::DroDatum, UiFocus::Diagnostics,
                                      UiFocus::About};
// The two read-only screens, which share every rule.
const UiFocus kReadOnlyScreens[] = {UiFocus::Diagnostics, UiFocus::About};

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
  // The carriage is at rest when the run is ordered (the lockout means it must
  // be), and motionActive becomes true while the run is genuinely in flight.
  UiContext running = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/true);
  UiContext idle = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/false);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));

  // The carriage reaches the stop on its own - no key event from the
  // operator - and the next context the display hands to UiState reflects
  // that motion is no longer active.

  // The very next arrow click must start a fresh run, NOT cancel a stale one.
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

TEST(UiStateJog, NaturallyCompletedRightRunStartsAFreshRunOnTheNextClick) {
  Rig r;
  UiContext running = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/true);
  UiContext idle = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/false);
  ASSERT_EQ(UiIntent::RunToRightStop, r.startRun(UiKey::Right, idle, running));
  EXPECT_EQ(UiIntent::RunToRightStop, r.click(UiKey::Right, idle));
}

TEST(UiStateJog, InFlightRunStillCancelsOnTheNextClick) {
  // Control for the reconciliation fix above: while motionActive genuinely
  // stays true (the run has not completed), the existing cancel-on-second-
  // click behaviour (spec §7) must be unchanged - the reconciliation must not
  // make every powered run uncancellable.
  Rig r;
  UiContext running = ctx(true, false, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, kLeftOnly, running));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, running));
}

TEST(UiStateJog, InFlightRightRunStillCancelsOnTheNextClick) {
  Rig r;
  UiContext running = ctx(false, true, /*motionEnabled=*/false, /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToRightStop,
            r.startRun(UiKey::Right, kRightOnly, running));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Right, running));
}

// The two cases above pin THAT the latch is reconciled, but not WHERE. Both
// observe it through an arrow click, which would behave identically if the
// reconciliation lived next to its only reader inside the Jog branch. The two
// below pin the placement - at the top of handleKey, above every early return -
// which is the whole point of it: a run ends while the operator is somewhere
// else entirely, and the keypress that happens to be in flight at that moment
// must still clean up.
//
// Each works by making the LATER arrow click see motionActive true again (an
// unrelated jog, an engaged feed, a deceleration tail). If reconciliation only
// happened there, that click would see Confirmed + active, keep the stale latch
// and return CancelMotion. Getting None instead - the motion lockout's answer
// for an arrow with no run to stop - proves the earlier, unrelated key event is
// what cleared it. (Before the lockout the discriminator was a FRESH run rather
// than None; an arrow may no longer start one under power, so the observation
// moved but the thing observed did not.)

TEST(UiStateJog, AnInertNonArrowKeyAlsoReconcilesACompletedRun) {
  Rig r;
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  const UiContext idle = ctx(true, false, /*motionEnabled=*/false,
                             /*motionActive=*/false);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));

  // The run reaches the stop. The only event that sees the machine at rest is
  // an OK Press, which produces no intent at all - and must still reconcile.
  ASSERT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Press, idle));

  // Something is moving again by the time the operator reaches for an arrow. A
  // stale latch would answer CancelMotion here.
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, running));
  // ...and the machine is genuinely usable afterwards: at rest, a fresh run.
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

TEST(UiStateJog, ARunThatEndsWhileTheMenuIsOpenIsStillReconciled) {
  // The case the placement was chosen for: the menu branch returns early for
  // every event it does not use, so a reconciliation below it would never run
  // while the carousel is up.
  Rig r;
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  const UiContext idle = ctx(true, false, /*motionEnabled=*/false,
                             /*motionActive=*/false);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));

  r.click(UiKey::Menu, idle);  // run completes while the operator is in here
  ASSERT_TRUE(r.ui().menuOpen());
  ASSERT_EQ(UiIntent::CloseMenu, r.click(UiKey::Menu, idle));

  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, running))
      << "a stale latch would answer CancelMotion";
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
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

TEST(UiStateOk, ZeroDroIsInertUnderPower) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // ZeroDroStillWorksWhileMotionEnabled, on the reasoning that zeroing the DRO
  // moves no metal so the §3 arrow inhibit need not extend to it. The owner's
  // ruling supersedes that: OK is not a stop function, so under power it does
  // nothing at all. Zeroing the datum is also a thing you do to set up a cut,
  // not during one, so nothing is lost but the ability to do it by accident.
  const bool flags[] = {false, true};
  for (bool enabled : flags) {
    for (bool active : flags) {
      if (!enabled && !active) {
        continue;  // at rest - HoldAtRestZerosTheDroAndKeepsJogFocus covers it
      }
      Rig r;
      const UiContext c = ctx(false, false, enabled, active);
      EXPECT_EQ(UiIntent::None, r.hold(UiKey::Ok, c))
          << "motionEnabled=" << enabled << " motionActive=" << active;
      EXPECT_EQ(UiFocus::Jog, r.focus());
    }
  }
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

// ---------------------------------------------------------------------------
// THE SELECTOR TOGGLE (OWNER RULING, and a change from the first cut).
//
//   "menu opens the menu. pressing a second time closes it. The same logic
//    should apply to rate, mode, and stops"
//
// This used to be a no-op that merely restarted the idle timer, read off §1's
// list of leave conditions (OK / HALT / 4 s idle - not the key itself). That
// made MENU the odd one out of four otherwise identical keys. All four now
// toggle: a selector pressed while its OWN widget is open returns focus to Jog.
//
// Pressing a DIFFERENT selector still crosses straight over - that is the test
// immediately above, and it is deliberately kept as the other half of this
// contract, so a state machine that closed to Jog on every selector press would
// fail there rather than passing both.
// ---------------------------------------------------------------------------

TEST(UiStateSelectors, ModeClosesItsOwnWidgetOnASecondPress) {
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Mode));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateSelectors, RateClosesItsOwnWidgetOnASecondPress) {
  Rig r;
  r.click(UiKey::Rate);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Rate));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateSelectors, TheToggleIsAToggleAndNotAOneWayTrip) {
  // Third press re-opens. Without this the two tests above would pass on a
  // selector that had simply stopped taking focus after the first use.
  const UiKey selectors[] = {UiKey::Mode, UiKey::Rate, UiKey::Stops};
  const UiFocus focuses[] = {UiFocus::Mode, UiFocus::Rate, UiFocus::Stops};
  for (int i = 0; i < 3; ++i) {
    Rig r;
    r.click(selectors[i], kNoStops);
    ASSERT_EQ(focuses[i], r.focus()) << "open";
    r.click(selectors[i], kNoStops);
    ASSERT_EQ(UiFocus::Jog, r.focus()) << "close";
    r.click(selectors[i], kNoStops);
    EXPECT_EQ(focuses[i], r.focus()) << "re-open";
  }
}

TEST(UiStateSelectors, TheToggleDoesNotReachAcrossToAnotherWidget) {
  // Only the selector matching the CURRENT focus closes. MODE pressed while
  // RATE is open switches; it does not close to Jog, and it does not close
  // RATE's widget and leave nothing open.
  Rig modeThenRate;
  modeThenRate.click(UiKey::Mode);
  modeThenRate.click(UiKey::Rate);
  EXPECT_EQ(UiFocus::Rate, modeThenRate.focus());

  Rig rateThenMode;
  rateThenMode.click(UiKey::Rate);
  rateThenMode.click(UiKey::Mode);
  EXPECT_EQ(UiFocus::Mode, rateThenMode.focus());

  // ...and the same across the two focuses a selector cannot re-enter: OK's
  // JogSpeed widget is not MODE's, so MODE opens over it rather than closing.
  Rig fromJogSpeed;
  fromJogSpeed.click(UiKey::Ok);
  ASSERT_EQ(UiFocus::JogSpeed, fromJogSpeed.focus());
  fromJogSpeed.click(UiKey::Mode);
  EXPECT_EQ(UiFocus::Mode, fromJogSpeed.focus());
}

TEST(UiStateSelectors, MenuIsUnchangedByTheSelectorToggle) {
  // MENU already worked this way (§6) and is the key the ruling generalises
  // FROM, so it must come through untouched: open, close, re-open at index 0,
  // and CloseMenu still emitted on the closing press (the selectors emit None
  // either way - opening a picker was never an intent).
  Rig r;
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu));
  ASSERT_TRUE(r.ui().menuOpen());
  ASSERT_EQ(UiFocus::Menu, r.focus());
  EXPECT_EQ(UiIntent::CloseMenu, r.click(UiKey::Menu));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu));
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateSelectors, AreInertUnderPower) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // SelectorsStillWorkWhileMotionEnabled, which pinned "RATE + arrows changes
  // pitch mid-cut" on the reasoning that the §3 inhibit was about the carriage
  // and not the widgets. The owner's ruling supersedes it: no selector may open
  // a widget while the carriage is moving, so the arrows never get a widget to
  // drive either.
  //
  // Every selector, both motion flags, and the focus checked as well as the
  // intent - a selector that returned None but still moved focus would leave a
  // picker on screen mid-cut, which is the exact thing the ruling is about.
  const UiKey selectors[] = {UiKey::Mode, UiKey::Rate, UiKey::Stops};
  const bool flags[] = {false, true};
  for (UiKey k : selectors) {
    for (bool enabled : flags) {
      for (bool active : flags) {
        if (!enabled && !active) {
          continue;  // at rest - the selectors above pin the live behaviour
        }
        Rig r;
        const UiContext c = ctx(false, false, enabled, active);
        EXPECT_EQ(UiIntent::None, r.click(k, c))
            << "enabled=" << enabled << " active=" << active;
        EXPECT_EQ(UiFocus::Jog, r.focus())
            << "no widget may open under power";
        // ...and with no widget open the arrows cannot step a setting either.
        EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, c));
        EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, c));
      }
    }
  }
}

TEST(UiStateSelectors, AreLiveAgainOnceTheCarriageIsAtRest) {
  // The other direction of the contract: without it the lockout test above
  // would pass on a state machine that had simply broken the selectors.
  Rig r;
  const UiContext moving = ctx(false, false, /*motionEnabled=*/true);
  ASSERT_EQ(UiIntent::None, r.click(UiKey::Rate, moving));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  r.click(UiKey::Rate, kNoStops);
  EXPECT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::PitchNext, r.click(UiKey::Right, kNoStops));
}

// ===========================================================================
// 5. STOPS asymmetry (spec §4 - the safety-relevant one)
// ===========================================================================

TEST(UiStateStops, LeftClickWithLeftStopUnsetSetsIt) {
  // GitHub issue #10: setting a stop now dismisses the widget too. After
  // setting, the operator's next move is almost always to jog or engage, so
  // the natural expectation is that the screen has already gone - the same
  // destination every other dismiss path uses (OK, HALT, ENABLE, the idle
  // timeout). This used to assert UiFocus::Stops (the widget stayed up); that
  // was the bug the issue is about, not a contract worth keeping.
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetLeftStop, r.click(UiKey::Left, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateStops, LeftClickWithLeftStopSetDoesNothing) {
  // "Flash 'hold to clear' - no action." Also (issue #10): a refused/no-op
  // click is NOT "a stop was set", so it must not auto-close the widget either
  // - only an actual SetLeftStop/SetRightStop does that.
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, LeftHoldWithLeftStopSetClearsIt) {
  // Issue #10: clearing is not setting, so this must NOT auto-close - the
  // widget stays up over the result exactly as it always has.
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::ClearLeftStop, r.hold(UiKey::Left, kLeftOnly));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, LeftHoldWithLeftStopUnsetDoesNothing) {
  // Decision: hold means "clear"; with nothing to clear it is inert rather
  // than falling back to set (a hold must never set a stop by accident).
  // Issue #10: an inert gesture is not "a stop was set" either, so it must not
  // auto-close.
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Left, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, RightClickWithRightStopUnsetSetsIt) {
  // Issue #10: setting the right stop closes the widget too, same as left.
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetRightStop, r.click(UiKey::Right, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateStops, RightClickWithRightStopSetDoesNothing) {
  Rig r;
  r.click(UiKey::Stops, kRightOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, kRightOnly));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, RightHoldWithRightStopSetClearsIt) {
  Rig r;
  r.click(UiKey::Stops, kRightOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::ClearRightStop, r.hold(UiKey::Right, kRightOnly));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, RightHoldWithRightStopUnsetDoesNothing) {
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Right, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStops, EachArrowOnlySeesItsOwnStop) {
  // Left stop set, right unset: left click is inert (stays open, issue #10),
  // right click sets and closes.
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, kLeftOnly));
  ASSERT_EQ(UiFocus::Stops, r.focus())
      << "an inert click on an already-set stop must not auto-close";
  EXPECT_EQ(UiIntent::SetRightStop, r.click(UiKey::Right, kLeftOnly));
  EXPECT_EQ(UiFocus::Jog, r.focus());
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

// SETUP NOTE for this whole block. Since the motion lockout, the STOPS key
// cannot take focus under power at all, so these tests open the widget AT REST
// and only then hand the arrows a powered context. That is not a contrivance:
// it is the one way the state is still reachable on the real machine - the web
// UI and a spindle-driven feed can both start the carriage while a picker is
// open - and it is what keeps the stop-edit gate below the lockout falsifiable
// rather than merely unreachable.

TEST(UiStateStops, ClickDoesNotSetAStopWhileMotionEnabled) {
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    Rig r;
    UiContext c = ctx(false, false, /*motionEnabled=*/true);
    r.click(UiKey::Stops, kNoStops);  // opened at rest; motion starts under it
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
    r.click(UiKey::Stops, kBothStops);
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
          r.click(UiKey::Stops, ctx(ls, rs));  // at rest
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
  // The other direction of the contract: the inhibit is a state, not a latch,
  // so the identical gestures work once disengaged. Without this the tests
  // above would pass on a state machine that had simply broken STOPS.
  const UiContext engaged = ctx(false, false, true);
  const UiContext engagedSet = ctx(true, true, true);

  Rig setL;
  setL.click(UiKey::Stops, kNoStops);
  ASSERT_EQ(UiIntent::None, setL.click(UiKey::Left, engaged));
  EXPECT_EQ(UiIntent::SetLeftStop, setL.click(UiKey::Left, kNoStops));

  Rig setR;
  setR.click(UiKey::Stops, kNoStops);
  ASSERT_EQ(UiIntent::None, setR.click(UiKey::Right, engaged));
  EXPECT_EQ(UiIntent::SetRightStop, setR.click(UiKey::Right, kNoStops));

  Rig clearL;
  clearL.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiIntent::None, clearL.hold(UiKey::Left, engagedSet));
  EXPECT_EQ(UiIntent::ClearLeftStop, clearL.hold(UiKey::Left, kBothStops));

  Rig clearR;
  clearR.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiIntent::None, clearR.hold(UiKey::Right, engagedSet));
  EXPECT_EQ(UiIntent::ClearRightStop, clearR.hold(UiKey::Right, kBothStops));
}

TEST(UiStateStops, TheStopsWidgetDoesNotOpenWhileMotionEnabled) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // TheStopsWidgetStillOpensWhileMotionEnabled, on the reasoning that only the
  // edits were inhibited and the travel bar was worth having on screen mid-cut.
  // The owner's ruling supersedes it: STOPS is not a stop function, so under
  // power it does nothing - no focus change, no widget, nothing to time out.
  // (The travel bar is not lost; it is on the rest screen, which is what the
  // operator is now left looking at.)
  Rig r;
  UiContext c = ctx(true, true, /*motionEnabled=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops, c));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  // Nothing was opened, so the idle timeout has nothing to expire.
  r.advance(kTimeout);
  EXPECT_FALSE(r.tick());
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
    r.click(UiKey::Stops, kNoStops);  // opened at rest - see the setup note
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
    r.click(UiKey::Stops, kBothStops);  // opened at rest - see the setup note
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
          r.click(UiKey::Stops, ctx(ls, rs));  // at rest - see the setup note
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

TEST(UiStateStops, TheStopsWidgetDoesNotOpenWhileMotionActive) {
  // CHANGED BY THE MOTION LOCKOUT, for the same reason as the motionEnabled
  // case above: a powered run is still "moving", and the ruling is about the
  // operator's attention, not about which motion mode the machine is in.
  Rig r;
  UiContext c = ctx(true, true, /*motionEnabled=*/false, /*motionActive=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops, c));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  r.advance(kTimeout);
  EXPECT_FALSE(r.tick());
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
  EXPECT_FALSE(ui.tick(c, justBefore));
  EXPECT_EQ(UiFocus::Mode, ui.focus());

  // ...and fires at exactly kFocusTimeoutMs across the wrap.
  EXPECT_TRUE(ui.tick(c, kNearMax + kTimeout));
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

TEST(UiStateMenu, IndexWrapsAtTheStart) {
  // WRAPPING, not saturating. Owner's reason: "there's nothing worse than
  // cycling through to something you know is at the end". Six tiles on a
  // full-width carousel means the far end is five presses away the wrong way,
  // and one press the right way.
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_EQ(0, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuPrev, r.click(UiKey::Left));
  EXPECT_EQ(kMenuItems - 1, r.ui().menuIndex()) << "left from the first tile lands on the last";
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
  // And straight back, so the wrap is symmetric rather than a one-way jump.
  EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right));
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateMenu, IndexWrapsAtTheEnd) {
  Rig r;
  r.click(UiKey::Menu);
  for (int i = 1; i < kMenuItems; ++i) {
    EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right)) << "step " << i;
    EXPECT_EQ(i, r.ui().menuIndex());
  }
  EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right));
  EXPECT_EQ(0, r.ui().menuIndex()) << "right from the last tile lands on the first";
  // Keeps going round rather than sticking or running off the end.
  r.click(UiKey::Right);
  EXPECT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuPrev, r.click(UiKey::Left));
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateMenu, AFullLapReturnsToWhereItStarted) {
  // The index must stay inside [0, kMenuItems) for every step of a lap in
  // both directions - the carousel renders straight from it, so an index one
  // past either end would read or dispatch a tile that does not exist.
  Rig r;
  r.click(UiKey::Menu);
  for (int i = 0; i < kMenuItems; ++i) {
    r.click(UiKey::Right);
    EXPECT_GE(r.ui().menuIndex(), 0) << "step " << i;
    EXPECT_LT(r.ui().menuIndex(), kMenuItems) << "step " << i;
  }
  EXPECT_EQ(0, r.ui().menuIndex()) << "a full lap returns to the start";
  for (int i = 0; i < kMenuItems; ++i) {
    r.click(UiKey::Left);
    EXPECT_GE(r.ui().menuIndex(), 0) << "step " << i;
    EXPECT_LT(r.ui().menuIndex(), kMenuItems) << "step " << i;
  }
  EXPECT_EQ(0, r.ui().menuIndex()) << "and the same the other way";
}

// UPDATED (was OkActivatesTheCurrentTileAndLeavesTheMenuOpen). The old contract
// - "tiles toggle in place, so activation must not dismiss the carousel" - is
// the exact behaviour the owner overturned: the carousel is a full-width panel
// covering the pitch, the theme and the travel bar, i.e. covering everything a
// tile changes, so leaving it up meant OK visibly did nothing. The new rule is
// "OK always closes the menu, and you always land somewhere that shows the
// result". The refused-tile exception is pinned separately below.
TEST(UiStateMenu, OkActivatesTheCurrentTileAndClosesTheMenu) {
  Rig r;
  r.click(UiKey::Menu, kThreadIdle);
  r.click(UiKey::Right, kThreadIdle);
  ASSERT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuActivate, r.click(UiKey::Ok, kThreadIdle));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus()) << "Theme's result is the whole screen";
}

TEST(UiStateMenu, ActivateLeavesTheIndexReadableToCaller) {
  // Load-bearing contract, not incidental: ButtonPad::activateMenuTile() reads
  // menuIndex() AFTER handleKey() has returned, to decide which tile to
  // execute. If closing the carousel reset the index, every tile would fire
  // tile 0 - Units - instead of the one that was selected.
  Rig r;
  ASSERT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_SOFTWARE_UPDATE));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ((int)MENU_SOFTWARE_UPDATE, r.ui().menuIndex());
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

// ===========================================================================
// 7. menuTileBlock() - the menu-tile availability rule (spec §6).
//
// Moved here from lib/display/ST7789_320_240displaylvgl.h so the rule that
// gates Sync / Software update / Wi-Fi setup / Theme / DRO datum can be host-
// tested at all: it used to live behind <lvgl.h>, which the native runner
// cannot build. Pure function of (tile, motionActive, threadMode); no Rig,
// no clock, no UiState instance needed.
// ===========================================================================

TEST(UiStateMenuTileBlock, TileCountMatchesUiState) {
  // The header-level static_assert already enforces this at compile time; this
  // is the same check made falsifiable at the unit level, and it is the one
  // that fails if a tile is appended to MenuTile without kMenuItemCount (or
  // vice versa) being updated to match.
  EXPECT_EQ((int)MENU_TILE_COUNT, kMenuItems);
}

TEST(UiStateMenuTileBlock, EveryTileReturnsADefinedValueAtRestAndUnderPower) {
  // Falsifiable coverage for "a tile was added to MenuTile without a rule":
  // walk every index the enum defines, in both a rest and a powered context,
  // and require the answer to be one of the three known verdicts. A tile that
  // fell through to some other value (or crashed / UB'd) would fail this.
  for (int tile = 0; tile < MENU_TILE_COUNT; ++tile) {
    for (bool motionActive : {false, true}) {
      for (bool threadMode : {false, true}) {
        const MenuTileBlock block = menuTileBlock(tile, motionActive, threadMode);
        EXPECT_TRUE(block == MTB_NONE || block == MTB_MOTION ||
                    block == MTB_FEED_MODE)
          << "tile " << tile << " motionActive=" << motionActive
          << " threadMode=" << threadMode << " returned undefined value "
          << (int)block;
      }
    }
  }
}

TEST(UiStateMenuTileBlock, SyncBlockedByMotionRegardlessOfThreadMode) {
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_SYNC, true, /*threadMode=*/false));
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_SYNC, true, /*threadMode=*/true));
}

TEST(UiStateMenuTileBlock, SyncBlockedByFeedModeWhenAtRest) {
  // At rest (not under power) but NOT in a thread mode - i.e. plain feed mode.
  EXPECT_EQ(MTB_FEED_MODE, menuTileBlock(MENU_SYNC, false, /*threadMode=*/false));
}

TEST(UiStateMenuTileBlock, SyncAllowedAtRestInEitherThreadMode) {
  // threadMode collapses FM_THREAD (right-hand) and FM_THREAD_REVERSE
  // (left-hand) to one bool at the call sites (ButtonPad::activateMenuTile(),
  // Display::drawOverlay()) - menuTileBlock() itself only ever sees that one
  // bool, so both directions land on this single `true` case. Asserted once
  // for right-hand thread and once again to record explicitly that left-hand
  // thread is the SAME call, not a distinct one this function forgot to make.
  const bool rightHandThread = true;   // FM_THREAD
  const bool leftHandThread = true;    // FM_THREAD_REVERSE (same bool)
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_SYNC, false, rightHandThread));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_SYNC, false, leftHandThread));
}

TEST(UiStateMenuTileBlock, SyncPrecedenceUnderPowerAndInFeedModeIsMotion) {
  // Both conditions apply at once: under power AND not in a thread mode.
  // The implementation checks motionActive first and returns MTB_MOTION
  // without ever consulting threadMode - i.e. "stop the carriage" wins over
  // "needs thread mode" when both are true. Pinning that precedence, not
  // changing it (task instructions: assert what the implementation does).
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_SYNC, true, /*threadMode=*/false));
}

TEST(UiStateMenuTileBlock, ThemeBlockedOnlyByMotion) {
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_THEME, true, false));
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_THEME, true, true));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_THEME, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_THEME, false, true));
}

TEST(UiStateMenuTileBlock, DroDatumBlockedOnlyByMotion) {
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_DRO_DATUM, true, false));
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_DRO_DATUM, true, true));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_DRO_DATUM, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_DRO_DATUM, false, true));
}

TEST(UiStateMenuTileBlock, SoftwareUpdateBlockedOnlyByMotion) {
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_SOFTWARE_UPDATE, true, false));
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_SOFTWARE_UPDATE, true, true));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_SOFTWARE_UPDATE, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_SOFTWARE_UPDATE, false, true));
}

TEST(UiStateMenuTileBlock, WifiSetupBlockedOnlyByMotion) {
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_WIFI_SETUP, true, false));
  EXPECT_EQ(MTB_MOTION, menuTileBlock(MENU_WIFI_SETUP, true, true));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_WIFI_SETUP, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_WIFI_SETUP, false, true));
}

TEST(UiStateMenuTileBlock, UnitsNeverBlockedByMotionOrFeedMode) {
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_UNITS, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_UNITS, true, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_UNITS, true, true));
}

TEST(UiStateMenuTileBlock, JogSpeedNeverBlockedByMotionOrFeedMode) {
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_JOG_SPEED, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_JOG_SPEED, true, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_JOG_SPEED, true, true));
}

TEST(UiStateMenuTileBlock, DiagnosticsNeverBlockedByMotionOrFeedMode) {
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_DIAGNOSTICS, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_DIAGNOSTICS, true, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_DIAGNOSTICS, true, true));
}

TEST(UiStateMenuTileBlock, AboutNeverBlockedByMotionOrFeedMode) {
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_ABOUT, false, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_ABOUT, true, false));
  EXPECT_EQ(MTB_NONE, menuTileBlock(MENU_ABOUT, true, true));
}

// ===========================================================================
// 10. The rotary encoder, routed through the focus model
//
// Before this, KeyArray::updateEncoderPos() called GlobalState::next/
// prevFeedPitch() directly, so the knob stepped the pitch from ANY focus -
// including from inside the STOPS widget and the menu. It was masked by the old
// button lock; the Mk2 panel unlocks at boot, so it was live from power-on.
//
// The owner's mapping, which these tests are the executable copy of:
//   Jog (at rest)  pitch   <- NOT what the arrows do here (they jog)
//   Rate           pitch
//   JogSpeed       jog speed
//   Mode           mode
//   Menu           move between tiles
//   Stops          INERT   <- a knob is far easier to nudge than a key, and
//                             setting an endstop stays a deliberate keypress
//
// ...and, on top of that mapping, one blanket rule: while the carriage is under
// power the knob is inert in EVERY focus (owner ruling - see the
// UiStateEncoderInhibit block below). The mapping above is what the knob does
// at rest.
// ===========================================================================

UiIntent turn(Rig& r, bool cw, const UiContext& c = kNoStops) {
  return r.key(cw ? UiKey::EncoderCw : UiKey::EncoderCcw, UiKeyEvent::Click, c);
}

// What one clockwise detent means in each focus WHEN THE CARRIAGE IS AT REST.
// The table above, as data, so the inhibit tests can assert both directions of
// the contract - dead under power, and this again at rest - without either
// passing on a knob that is simply broken everywhere.
UiIntent restingCwIntent(UiFocus f) {
  switch (f) {
    case UiFocus::Jog: return UiIntent::PitchNext;
    case UiFocus::Rate: return UiIntent::PitchNext;
    case UiFocus::JogSpeed: return UiIntent::JogSpeedNext;
    case UiFocus::Mode: return UiIntent::ModeNext;
    case UiFocus::Menu: return UiIntent::MenuNext;
    case UiFocus::Stops: return UiIntent::None;  // inert at rest as well
  }
  return UiIntent::None;
}

TEST(UiStateEncoder, AtRestStepsThePitch) {
  // The gap the knob fills: at rest the arrows jog, so without this there is no
  // way to change pitch without first pressing RATE.
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::PitchNext, turn(r, true));
  EXPECT_EQ(UiIntent::PitchPrev, turn(r, false));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateEncoder, AtRestStepsThePitchAndNeverJogs) {
  // Whatever the stops say, the knob is not an actuator: it must never emit a
  // jog, a run to a stop, or a cancel.
  const UiContext contexts[] = {kNoStops, kLeftOnly, kRightOnly, kBothStops};
  for (const UiContext& c : contexts) {
    Rig r;
    EXPECT_EQ(UiIntent::PitchNext, turn(r, true, c));
    EXPECT_EQ(UiIntent::PitchPrev, turn(r, false, c));
    EXPECT_EQ(UiFocus::Jog, r.focus());
  }
}

TEST(UiStateEncoder, InRateFocusStepsThePitch) {
  Rig r;
  r.click(UiKey::Rate);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::PitchNext, turn(r, true));
  EXPECT_EQ(UiIntent::PitchPrev, turn(r, false));
  EXPECT_EQ(UiFocus::Rate, r.focus());
}

TEST(UiStateEncoder, InJogSpeedFocusStepsTheJogSpeed) {
  Rig r;
  r.click(UiKey::Ok);
  ASSERT_EQ(UiFocus::JogSpeed, r.focus());
  EXPECT_EQ(UiIntent::JogSpeedNext, turn(r, true));
  EXPECT_EQ(UiIntent::JogSpeedPrev, turn(r, false));
  EXPECT_EQ(UiFocus::JogSpeed, r.focus());
}

TEST(UiStateEncoder, InModeFocusStepsTheMode) {
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  EXPECT_EQ(UiIntent::ModeNext, turn(r, true));
  EXPECT_EQ(UiIntent::ModePrev, turn(r, false));
  EXPECT_EQ(UiFocus::Mode, r.focus());
}

TEST(UiStateEncoder, InTheMenuMovesBetweenTiles) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::MenuNext, turn(r, true));
  EXPECT_EQ(1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuNext, turn(r, true));
  EXPECT_EQ(2, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuPrev, turn(r, false));
  EXPECT_EQ(1, r.ui().menuIndex());
  EXPECT_TRUE(r.ui().menuOpen());
}

TEST(UiStateEncoder, InTheMenuWrapsAtBothEnds) {
  // The knob wraps exactly as the arrows do. A knob that stops dead at the
  // end of a carousel feels broken in a way a button does not.
  Rig r;
  r.click(UiKey::Menu);
  EXPECT_EQ(UiIntent::MenuPrev, turn(r, false));
  EXPECT_EQ(kMenuItems - 1, r.ui().menuIndex());

  EXPECT_EQ(UiIntent::MenuNext, turn(r, true));
  EXPECT_EQ(0, r.ui().menuIndex());

  for (int i = 0; i < kMenuItems - 1; i++) {
    EXPECT_EQ(UiIntent::MenuNext, turn(r, true)) << "step " << i;
  }
  EXPECT_EQ(kMenuItems - 1, r.ui().menuIndex());
  EXPECT_EQ(UiIntent::MenuNext, turn(r, true));
  EXPECT_EQ(0, r.ui().menuIndex());
}

// ---------------------------------------------------------------------------
// The safety-relevant one. STOPS is inert to the knob, always.
//
// This is not "the encoder happens not to be wired into STOPS": it is the one
// focus where the knob is deliberately dead. Every gesture in the STOPS widget
// either destroys a position the operator spent time finding or plants one in a
// place they never saw, and a knob is far easier to nudge accidentally than a
// key is to press. §4's click-sets / hold-clears asymmetry has no analogue on a
// knob, so the answer is that the knob does not touch stops at all.
//
// Note these run at REST (no motion flags), so the existing motion inhibit
// cannot be what produces the None - it has to be the focus rule itself.
// ---------------------------------------------------------------------------

TEST(UiStateEncoderStops, IsInertInTheStopsWidget) {
  const UiContext contexts[] = {kNoStops, kLeftOnly, kRightOnly, kBothStops};
  const bool directions[] = {true, false};
  for (const UiContext& c : contexts) {
    for (bool cw : directions) {
      Rig r;
      r.click(UiKey::Stops, c);
      ASSERT_EQ(UiFocus::Stops, r.focus());
      EXPECT_EQ(UiIntent::None, turn(r, cw, c))
          << "cw=" << cw << " left=" << c.leftStopSet
          << " right=" << c.rightStopSet;
      EXPECT_EQ(UiFocus::Stops, r.focus());
    }
  }
}

TEST(UiStateEncoderStops, NoEventOnTheKnobEverTouchesAStop) {
  // Exhaustive over direction, event and stop state, at rest. Nothing the
  // encoder can produce may set, clear, or otherwise act inside STOPS.
  const UiKey knobs[] = {UiKey::EncoderCw, UiKey::EncoderCcw};
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Click, UiKeyEvent::Hold};
  const bool stopStates[] = {false, true};
  for (UiKey k : knobs) {
    for (bool ls : stopStates) {
      for (bool rs : stopStates) {
        for (UiKeyEvent ev : events) {
          Rig r;
          UiContext c = ctx(ls, rs);
          r.click(UiKey::Stops, c);
          ASSERT_EQ(UiFocus::Stops, r.focus());
          EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
              << "cw=" << (k == UiKey::EncoderCw) << " event=" << ev
              << " left=" << ls << " right=" << rs;
        }
      }
    }
  }
}

TEST(UiStateEncoderStops, TheKnobWorksAgainAsSoonAsStopsIsLeft) {
  // The other direction of the contract, so the two tests above cannot both
  // pass on an encoder that is simply broken everywhere. OK dismisses the
  // widget; the very next detent steps the pitch.
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiIntent::None, turn(r, true, kBothStops));
  r.click(UiKey::Ok, kBothStops);
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::PitchNext, turn(r, true, kBothStops));
}

TEST(UiStateEncoder, IsInertOnEveryEventThatIsNotAClick) {
  // src/buttonpad.cpp emits exactly one Click per detent. Nothing else on these
  // two keys means anything - a detent is instantaneous, there is nothing to
  // hold and no Release to pair.
  const UiKey knobs[] = {UiKey::EncoderCw, UiKey::EncoderCcw};
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Hold};
  for (UiFocus f : kAllFocuses) {
    for (UiKey k : knobs) {
      for (UiKeyEvent ev : events) {
        Rig r;
        r.enterFocus(f);
        EXPECT_EQ(UiIntent::None, r.key(k, ev, kNoStops))
            << "focus=" << f << " event=" << ev;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The motion inhibit (OWNER RULING, and a change from the first cut of this
// feature).
//
// The knob used to inherit the arrows' inhibit focus by focus: dead at Jog while
// MM_ENABLED, live in Rate / Mode / JogSpeed. It is now dead EVERYWHERE while
// the carriage is under power. A knob is the input most easily disturbed by
// accident on a running machine, and - unlike the arrows, which must stay live
// because they cancel a powered run and terminate a dead-man jog - the knob has
// no job to do during motion, so the blanket rule costs no capability.
//
// The KEYS are unchanged. UiStateSelectors.SelectorsStillWorkWhileMotionEnabled
// still pins that RATE + arrows changes pitch mid-cut. Only the knob is dead.
//
// The predicate is motionEnabled OR motionActive - the same one the stop edits
// use. The engaged feed is not the only state worth protecting: the powered run
// to a stop, the interactive jog and the deceleration tail are all "the
// carriage is moving", and none of them are MM_ENABLED.
// ---------------------------------------------------------------------------

TEST(UiStateEncoderInhibit, IsDeadInEveryFocusWhileEngaged) {
  const bool directions[] = {true, false};
  const bool stopStates[] = {false, true};
  for (UiFocus f : kAllFocuses) {
    for (bool cw : directions) {
      for (bool s : stopStates) {
        Rig r;
        r.enterFocus(f);  // entered at rest, as an operator would
        ASSERT_EQ(f, r.focus());
        UiContext c = ctx(s, s, /*motionEnabled=*/true);
        EXPECT_EQ(UiIntent::None, turn(r, cw, c))
            << "focus=" << f << " cw=" << cw << " stopsSet=" << s;
      }
    }
  }
}

TEST(UiStateEncoderInhibit, IsDeadInEveryFocusWhileMotionActive) {
  // motionActive WITHOUT motionEnabled: the powered run to a stop (MM_JOG_*),
  // the interactive jog and the deceleration tail. This is the half the old
  // per-focus rule missed entirely, and it is the half where the carriage is
  // actually moving.
  const bool directions[] = {true, false};
  for (UiFocus f : kAllFocuses) {
    for (bool cw : directions) {
      Rig r;
      r.enterFocus(f);
      UiContext c = ctx(true, true, /*motionEnabled=*/false,
                        /*motionActive=*/true);
      EXPECT_EQ(UiIntent::None, turn(r, cw, c))
          << "focus=" << f << " cw=" << cw;
    }
  }
}

TEST(UiStateEncoderInhibit, IsLiveAgainInEveryFocusOnceTheCarriageIsAtRest) {
  // The other direction of the contract: without it the two tests above would
  // pass on a knob that had simply been unplugged. Same rig, same focus, the
  // only difference is the motion flags going false.
  //
  // Stops is skipped because it is inert at rest too (that is its own rule,
  // pinned by UiStateEncoderStops), so it cannot tell the two states apart.
  for (UiFocus f : kAllFocuses) {
    if (f == UiFocus::Stops) {
      continue;
    }
    Rig r;
    r.enterFocus(f);
    UiContext engaged = ctx(false, false, /*motionEnabled=*/true);
    ASSERT_EQ(UiIntent::None, turn(r, true, engaged)) << "focus=" << f;
    EXPECT_EQ(restingCwIntent(f), turn(r, true, kNoStops)) << "focus=" << f;
    EXPECT_EQ(f, r.focus()) << "focus=" << f;
  }
}

TEST(UiStateEncoderInhibit, RateFocusIsDeadWhileEngagedAndSoAreTheArrows) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // RateFocusIsDeadWhileEngagedButTheArrowsAreNot, and it existed to state the
  // accepted cost of the knob's blanket rule: the knob went dead inside a
  // widget the operator had opened on purpose, but the KEYS still changed pitch
  // mid-cut. The owner's ruling removes the exception - the widget cannot be
  // opened under power at all, so there is nothing for either input to drive.
  Rig r;
  UiContext c = ctx(false, false, /*motionEnabled=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Rate, c));
  ASSERT_EQ(UiFocus::Jog, r.focus()) << "RATE may not open a widget under power";
  EXPECT_EQ(UiIntent::None, turn(r, true, c));
  EXPECT_EQ(UiIntent::None, turn(r, false, c));
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, c));
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, c));
}

TEST(UiStateEncoderInhibit, TheMenuCannotEvenBeOpenedUnderPower) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // TheMenuCarouselIsAlsoInhibited, which pinned that the knob did not move the
  // carousel under power while the ARROWS still did, with menuTileBlock()
  // refusing the dangerous tiles at the OK. The owner's ruling is one level
  // above that: "it shouldn't be possible to open a menu or tile while moving",
  // so the carousel never appears and neither input has anything to walk.
  Rig r;
  UiContext c = ctx(false, false, /*motionEnabled=*/true);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu, c));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, turn(r, true, c));
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, c));
  EXPECT_EQ(0, r.ui().menuIndex());
}

TEST(UiStateEncoder, DoesNotCancelAPoweredRun) {
  // The knob is not an actuator, so it neither starts nor stops carriage
  // motion. Under the blanket inhibit a detent mid-run now does nothing at all
  // - but "nothing" has to mean nothing: it must not quietly drop the run
  // latch, so the arrows still cancel the run afterwards.
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  EXPECT_EQ(UiIntent::None, turn(r, true, running));
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, running));
}

TEST(UiStateEncoder, DoesNotChangeFocus) {
  for (UiFocus f : kAllFocuses) {
    Rig r;
    r.enterFocus(f);
    turn(r, true);
    EXPECT_EQ(f, r.focus()) << "focus=" << f;
    turn(r, false);
    EXPECT_EQ(f, r.focus()) << "focus=" << f;
  }
}

TEST(UiStateEncoder, ResetsTheIdleTimeout) {
  // A detent is activity like any other key event: turning the knob inside a
  // widget must keep that widget on screen.
  Rig r;
  r.click(UiKey::Rate);
  r.advance(kTimeout - 1);
  turn(r, true);
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick());
  EXPECT_EQ(UiFocus::Rate, r.focus());
  r.advance(1);
  EXPECT_TRUE(r.tick());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

// ===========================================================================
// 11. STOPS held: clear BOTH stops, behind a 1 s confirm bar (spec §4)
//
// The one second is KeyArray's existing hold timer (src/keyarray.cpp:58), so
// `Stops` + `Hold` IS the gesture - UiState adds no timer of its own, only the
// press bookkeeping that stopsConfirmPermille() reports for the bar.
//
// Clearing both is strictly worse than clearing one: the per-arrow clear leaves
// a stop for the helix to re-anchor onto, whereas clearing both leaves
// syncPositionState UNSET with no anchor at all, so the thread can never be
// picked up for a second cut.
// ===========================================================================

TEST(UiStateClearBoth, StopsHeldInsideTheWidgetClearsBoth) {
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::ClearBothStops, r.hold(UiKey::Stops, kBothStops));
}

TEST(UiStateClearBoth, OneStopSetIsEnough) {
  // "Clear both" means "leave no stops", not "there must be two".
  Rig rl;
  rl.click(UiKey::Stops, kLeftOnly);
  EXPECT_EQ(UiIntent::ClearBothStops, rl.hold(UiKey::Stops, kLeftOnly));

  Rig rr;
  rr.click(UiKey::Stops, kRightOnly);
  EXPECT_EQ(UiIntent::ClearBothStops, rr.hold(UiKey::Stops, kRightOnly));
}

TEST(UiStateClearBoth, DoesNothingWhenNoStopIsSet) {
  Rig r;
  r.click(UiKey::Stops, kNoStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Stops, kNoStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

// ---------------------------------------------------------------------------
// STOPS takes focus on the PRESS (OWNER RULING, and a change from the first cut
// of this feature).
//
// It used to take focus on the Click like MODE and RATE, and since the keypad
// emits no Click after a Hold, holding STOPS from the rest screen did nothing
// whatsoever - no focus change, no bar, no clear. Clear-both was reachable only
// as tap-then-hold, with the widget already open.
//
// Moving the focus change one event earlier makes press-and-hold work from the
// rest screen AND puts the widget on screen while the bar fills, over the stop
// markers and the travel bar the operator is about to lose. MODE and RATE stay
// Click-only: STOPS is the only selector whose Hold means anything, so it is
// the only one where Click-only focus loses a gesture.
// ---------------------------------------------------------------------------

TEST(UiStateClearBoth, StopsPressAloneTakesFocus) {
  // The mechanism, on its own. The Press is what opens the widget; the Click of
  // an ordinary tap then lands on an already-focused widget.
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateClearBoth, StopsHeldFromTheRestScreenClearsBoth) {
  // The ruling, end to end: press and hold from the rest screen, one gesture.
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::ClearBothStops, r.hold(UiKey::Stops, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateClearBoth, StopsHeldFromTheRestScreenNeedsAStopToClear) {
  // The gate is unchanged by opening on the Press: with nothing to clear the
  // hold still does nothing (and the bar below never fills for it).
  Rig r;
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Stops, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());  // it still OPENED the widget
}

TEST(UiStateClearBoth, StopsHeldFromTheRestScreenIsStillRefusedUnderPower) {
  // The at-rest gate is unchanged too. Opening the widget on the Press must not
  // have bought the gesture a way past the motion inhibit.
  const bool flags[] = {false, true};
  for (bool enabled : flags) {
    for (bool active : flags) {
      if (!enabled && !active) {
        continue;
      }
      Rig r;
      UiContext c = ctx(true, true, enabled, active);
      ASSERT_EQ(UiFocus::Jog, r.focus());
      EXPECT_EQ(UiIntent::None, r.hold(UiKey::Stops, c))
          << "motionEnabled=" << enabled << " motionActive=" << active;
    }
  }
}

TEST(UiStateClearBoth, StopsHeldWithTheMenuOpenDoesNothing) {
  // The one focus the Press does NOT take, and the reason it does not: the menu
  // branch swallows every non-Click event while the carousel is up (§6 - the
  // menu leaves on MENU or HALT only), so the press never reaches the STOPS
  // code at all. No focus theft, no bar, no clear.
  Rig r;
  r.click(UiKey::Menu, kBothStops);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Stops, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Menu, r.focus());
}

TEST(UiStateClearBoth, StopsHeldFromAnyOtherWidgetAlsoClearsBoth) {
  // The consequence of the ruling, spelled out: STOPS is a selector, so its
  // Press moves focus from wherever it was - including out of another widget -
  // and the hold then clears both. One rule, every non-menu focus.
  const UiFocus others[] = {UiFocus::Jog, UiFocus::JogSpeed, UiFocus::Rate,
                            UiFocus::Mode, UiFocus::Stops};
  for (UiFocus f : others) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    EXPECT_EQ(UiIntent::ClearBothStops, r.hold(UiKey::Stops, kBothStops))
        << "focus=" << f;
    EXPECT_EQ(UiFocus::Stops, r.focus()) << "focus=" << f;
  }
}

// ---------------------------------------------------------------------------
// The motion gate. Clearing both stops mid-motion is strictly worse than
// clearing one, so it is gated exactly like every other stop edit: the carriage
// must be at rest, and BOTH flags are consulted. motionEnabled alone would miss
// the powered run to a stop (MM_JOG_*), during which the stop being travelled
// towards is the run's ONLY arrest (leadscrew.cpp:233) - delete it and the run
// never terminates.
// ---------------------------------------------------------------------------

TEST(UiStateClearBoth, IsRefusedWhileTheCarriageIsUnderPower) {
  const bool flags[] = {false, true};
  for (bool enabled : flags) {
    for (bool active : flags) {
      if (!enabled && !active) {
        continue;  // at rest - covered by the allowed case below
      }
      Rig r;
      UiContext c = ctx(true, true, enabled, active);
      r.click(UiKey::Stops, kBothStops);  // opened at rest; motion starts under it
      ASSERT_EQ(UiFocus::Stops, r.focus());
      EXPECT_EQ(UiIntent::None, r.hold(UiKey::Stops, c))
          << "motionEnabled=" << enabled << " motionActive=" << active;
      EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()))
          << "and the bar must not be left pinned on screen";
    }
  }
}

TEST(UiStateClearBoth, IsRefusedIfMotionStartsDuringTheHold) {
  // The gate that actually matters is the one on the Hold, checked against the
  // FRESH context: a whole second passes between the press and the hold, and
  // the machine can be started inside it - by the web UI, by a spindle-driven
  // feed, by anything. A gate applied only when the press arrived would let
  // that second clear both stops out from under a moving carriage.
  const UiContext atRest = kBothStops;
  const UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.click(UiKey::Stops, atRest);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  r.key(UiKey::Stops, UiKeyEvent::Press, atRest);   // armed at rest
  ASSERT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  r.advance(UiState::kStopsConfirmMs);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, moving));
}

TEST(UiStateClearBoth, IsRefusedIfTheStopsVanishDuringTheHold) {
  // The Hold's OTHER re-check, and the only test that can see it. Every other
  // "no stops" case is refused one step earlier, by the Press declining to arm,
  // so deleting this half of the gate would leave them all green - the exact
  // untested-guard trap. Here the press arms legitimately (both stops set, at
  // rest) and the stops are then cleared from under it - by the web UI, or by
  // the arrows of a gesture that raced this one - so only the re-check on the
  // fresh context can refuse it.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(UiState::kStopsConfirmMs);
  ASSERT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, kNoStops));
}

TEST(UiStateClearBoth, IsAllowedOnceTheCarriageIsAtRest) {
  // The other direction of the contract: without this the refusals above would
  // pass on a gesture that had simply never been implemented.
  Rig r;
  UiContext moving = ctx(true, true, /*motionEnabled=*/true);
  r.click(UiKey::Stops, moving);
  ASSERT_EQ(UiIntent::None, r.hold(UiKey::Stops, moving));
  EXPECT_EQ(UiIntent::ClearBothStops, r.hold(UiKey::Stops, kBothStops));
}

TEST(UiStateClearBoth, DoesNotDisturbTheSingleStopGestures) {
  // Regression: STOPS gaining a Hold must not have changed what the ARROWS do
  // inside the widget (§4's click-sets / hold-clears asymmetry).
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  EXPECT_EQ(UiIntent::ClearLeftStop, r.hold(UiKey::Left, kBothStops));
  EXPECT_EQ(UiIntent::ClearRightStop, r.hold(UiKey::Right, kBothStops));
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, kBothStops));
  Rig r2;
  r2.click(UiKey::Stops, kNoStops);
  EXPECT_EQ(UiIntent::SetLeftStop, r2.click(UiKey::Left, kNoStops));
}

TEST(UiStateClearBoth, StopsTapFromTheRestScreenStillJustOpensTheWidget) {
  // The tap case, in the exact order ButtonPad's drain loop delivers it
  // (Press -> Click -> Release, all in one pass). Taking focus on the Press
  // must not have turned a tap into anything more than "open the widget": no
  // intent anywhere in the gesture, and no bar left behind.
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops));
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateClearBoth, AStopsClickThatLostItsPressStillOpensTheWidget) {
  // KeyArray can drop events (see the hazards in buttonpad.cpp). The Click arm
  // still sets focus, so a Click arriving without its Press is not a lost
  // gesture.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

// ---------------------------------------------------------------------------
// STOPS AND THE SELECTOR TOGGLE - the four cases, together, because they are
// only correct as a set.
//
// STOPS is the hard one. MODE and RATE take focus on the Click, so "a Click
// that finds its own focus already current is a second press" is the whole
// rule. STOPS takes focus on the PRESS (the ruling above), which means the
// Click of a FIRST tap also finds Stops focus already current - and closing
// there would make the widget flash and vanish inside one tap.
//
// The close therefore hangs off the Click AND is suppressed for the gesture
// that opened the widget. Two consequences, and each has its own trap:
//
//   * Click, not Press. KeyArray emits no Click after a Hold, so a long press
//     never reaches the closing branch and clear-both survives from BOTH
//     starting focuses. Close on the Press instead and case 4 below silently
//     dies - the widget shuts and the Hold lands on Jog focus.
//   * Suppressed for the opening press. Without that, case 1 dies - the tap
//     that is meant to open the widget closes it again in the same gesture.
//
// So all four are pinned here in one place: break either half and exactly one
// pair of these fails.
// ---------------------------------------------------------------------------

TEST(UiStateStopsToggle, CaseOneTapFromJogOpensAndStaysOpen) {
  // Press -> Click -> Release in the order ButtonPad's drain loop delivers it.
  // No intent anywhere, no bar left behind, and the widget is UP at the end.
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus())
      << "the Click of the opening tap must not close what its own Press "
         "opened";
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateStopsToggle, CaseTwoTapFromStopsClosesTheWidget) {
  // The ruling itself, for STOPS. The widget is already open, so this press did
  // not open it, so its Click closes.
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus()) << "the Press alone must not close it";
  // The Press of this tap armed the bar (both stops set, at rest), and it is
  // genuinely filling by the time the Click lands - the drain loop is fast, but
  // the display polls in between.
  r.advance(300);
  ASSERT_EQ(300, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  // Closing must take the bar with it: a confirm bar over the rest screen would
  // be drawn for a widget that is no longer there.
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateStopsToggle, CaseThreeHoldFromJogOpensFillsAndClearsBoth) {
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  ASSERT_EQ(UiFocus::Stops, r.focus()) << "the bar needs its widget on screen";
  r.advance(UiState::kStopsConfirmMs / 2);
  EXPECT_EQ(500, r.ui().stopsConfirmPermille(r.now()));
  r.advance(UiState::kStopsConfirmMs / 2);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::ClearBothStops,
            r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateStopsToggle, CaseFourHoldFromStopsStillFillsAndStillClearsBoth) {
  // THE ONE THE TOGGLE COULD HAVE BROKEN. With the widget already open, a long
  // press is Press -> Hold -> Release and there is no Click in it, so the
  // closing branch is never reached and the gesture is untouched. Closing on
  // the Press instead would shut the widget here and the Hold would fire into
  // Jog focus with the bar already gone.
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  ASSERT_EQ(UiFocus::Stops, r.focus()) << "the widget must still be up";
  r.advance(UiState::kStopsConfirmMs / 2);
  EXPECT_EQ(500, r.ui().stopsConfirmPermille(r.now()));
  r.advance(UiState::kStopsConfirmMs / 2);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::ClearBothStops,
            r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
}

TEST(UiStateStopsToggle, AnotherSelectorStillCrossesStraightOver) {
  // The toggle must not have turned "switch widget" into "close, then open".
  // Both directions across the STOPS boundary, in one press each.
  Rig intoStops;
  intoStops.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, intoStops.focus());
  intoStops.click(UiKey::Stops, kNoStops);
  EXPECT_EQ(UiFocus::Stops, intoStops.focus());

  Rig outOfStops;
  outOfStops.click(UiKey::Stops, kNoStops);
  ASSERT_EQ(UiFocus::Stops, outOfStops.focus());
  outOfStops.click(UiKey::Rate, kNoStops);
  EXPECT_EQ(UiFocus::Rate, outOfStops.focus());
}

TEST(UiStateStopsToggle, TheOpeningPressMarkerDoesNotOutliveItsPress) {
  // The suppression is scoped to ONE press. A tap opens the widget (marker set
  // by the Press, cleared by the Release); the very next tap must therefore
  // close, not be suppressed a second time.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateStopsToggle, TheMarkerIsClearedByAnEventOnAnyOtherKey) {
  // The self-heal path, and the reason the marker is cleared at the top of
  // handleKey beside the confirm bar. A STOPS press opens the widget; another
  // key then intervenes (which on the real panel means the STOPS Release was
  // dropped - KeyArray's debounce and its one-second rescan can both swallow
  // one). A Click arriving afterwards must not still be suppressed by a marker
  // belonging to a press that is long over.
  //
  // MENU is used as the interloper because it leaves focus back on Jog, so the
  // widget is genuinely re-opened by the Click rather than merely still open.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);  // marker set
  ASSERT_EQ(UiFocus::Stops, r.focus());
  r.click(UiKey::Menu, kBothStops);                    // marker must clear here
  r.click(UiKey::Menu, kBothStops);                    // ...and back to Jog
  ASSERT_EQ(UiFocus::Jog, r.focus());
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);  // lost its Press
  ASSERT_EQ(UiFocus::Stops, r.focus()) << "a stray Click still opens";
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  EXPECT_EQ(UiFocus::Jog, r.focus()) << "and the next one closes";
}

TEST(UiStateStopsToggle, TheMarkerDoesNotSurviveTheMotionLockout) {
  // A press that opened the widget at rest, whose Release then arrives under
  // power and is swallowed by the lockout. The marker belongs to that dead
  // press and must not be left standing, or a later Click - one whose own Press
  // was dropped - would read it as "my own press opened this" and refuse to
  // close a widget it never opened.
  const UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);  // at rest: marker set
  ASSERT_EQ(UiFocus::Stops, r.focus());
  r.key(UiKey::Stops, UiKeyEvent::Release, moving);    // swallowed by the lockout
  ASSERT_EQ(UiFocus::Stops, r.focus()) << "the lockout moves no focus";
  // Back at rest, and a Click that lost its Press.
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateStopsToggle, TheMarkerDoesNotSurviveCloseOnMotion) {
  // Same staleness, via tick()'s close-on-motion rather than a key event: the
  // widget is closed out from under a live press (motion can start with no key
  // event at all - the web UI, a spindle-driven feed). The marker described
  // that widget and must go with it.
  const UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);  // marker set
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_TRUE(r.tick(moving));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  // At rest again. A Click that lost its Press re-opens...
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  // ...and the next one must close it, which a stale marker would prevent.
  r.key(UiKey::Stops, UiKeyEvent::Click, kBothStops);
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

// ---------------------------------------------------------------------------
// AUTO-CLOSE ON SETTING A STOP (GitHub issue #10).
//
// "Setting a stop should dismiss the STOPS widget automatically. Today it
// stays up and needs a separate press to close. After setting a stop the
// operator almost always wants to jog or engage next, so the natural
// expectation is that the screen has already gone."
//
// The trigger is narrow and deliberate: ONLY an actual SetLeftStop /
// SetRightStop closes the widget. Every other gesture reachable from
// UiFocus::Stops - an inert click on an already-set stop, a hold that clears,
// a hold that finds nothing to clear, a blocked edit under power - is NOT "a
// stop was set" and must leave the widget exactly as it was. Those are pinned
// alongside each gesture above (LeftClickWithLeftStopSetDoesNothing etc.);
// this block covers the cross-cutting concerns: the destination (Jog, the
// same one every other dismiss path uses), the two pieces of STOPS-local
// latched state (m_stopsConfirming, m_stopsOpenedByPress - see the notes on
// both in uistate.h), and that the close is a REAL return to the rest screen
// and not a display-only illusion.
// ---------------------------------------------------------------------------

TEST(UiStateStopsAutoClose, ABlockedEditUnderPowerDoesNotCloseTheWidget) {
  // Stop edits are inert under power (stopEditsInhibited() -> underPower()),
  // per §4 and the motion lockout. A blocked edit is certainly not "a stop was
  // set", so it must not close the widget either - refusing the edit and then
  // dismissing the screen out from under the operator would be a confusing
  // double failure, on top of throwing away the "stop the carriage first"
  // hint the display has nowhere else to show.
  const bool flags[] = {false, true};
  for (bool enabled : flags) {
    for (bool active : flags) {
      if (!enabled && !active) {
        continue;  // at rest - the setting tests above cover this
      }
      Rig r;
      UiContext c = ctx(false, false, enabled, active);
      r.click(UiKey::Stops, kNoStops);  // opened at rest - see the setup note
      ASSERT_EQ(UiFocus::Stops, r.focus());
      EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, c))
          << "motionEnabled=" << enabled << " motionActive=" << active;
      EXPECT_EQ(UiFocus::Stops, r.focus())
          << "a blocked edit must not close the widget either "
          << "(motionEnabled=" << enabled << " motionActive=" << active << ")";
    }
  }
}

TEST(UiStateStopsAutoClose, TheOpeningPressMarkerDoesNotSurviveTheAutoClose) {
  // The STOPS Press that opened the widget can still be "physically down" -
  // its own Click/Hold/Release not yet delivered - at the instant an arrow's
  // Click sets the other stop and the widget auto-closes. handleKey()'s
  // top-of-function self-heal (any event on a key other than Stops clears
  // m_stopsConfirming and m_stopsOpenedByPress - see uistate.cpp) already runs
  // for that arrow event, so by the time STOPS's own tardy Release (or a
  // dropped one, per the KeyArray hazards documented in buttonpad.cpp) shows
  // up there must be nothing left of the old press to resurrect: the Release
  // has to be inert, must not reopen Stops focus, and must not leave the
  // confirm bar armed.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kNoStops);  // opens; nothing to arm
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetLeftStop,
            r.key(UiKey::Left, UiKeyEvent::Click, kNoStops));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  // The original STOPS gesture's own trailing Release, arriving late.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Release, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus())
      << "a stray Release from the press that opened the widget must not "
         "resurrect it";
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()))
      << "and must not leave the confirm bar armed either";
}

TEST(UiStateStopsAutoClose, FreshStopsPressAfterTheAutoCloseOpensCleanly) {
  // Guards against a naive fix that leaves m_stopsOpenedByPress or
  // m_stopsConfirming stuck from the closed gesture: either would corrupt the
  // very next STOPS press - a bogus immediate close (this test), or a confirm
  // bar that starts already primed (the next test).
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetLeftStop, r.click(UiKey::Left, kNoStops));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  // A fresh press must open normally - exactly case one of the selector
  // toggle (UiStateStopsToggle.CaseOneTapFromJogOpensAndStaysOpen): the Click
  // of the SAME press that opened it must not immediately close it again.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Click, kNoStops));
  EXPECT_EQ(UiFocus::Stops, r.focus())
      << "the Click of this fresh press must not immediately close it";
}

TEST(UiStateStopsAutoClose, TheConfirmBarStartsCleanOnTheNextStopsPress) {
  // The other half of the guard above: if the auto-close left m_stopsConfirming
  // stuck true, the NEXT press's own arming logic (which only ever sets it,
  // never clears it first - see the STOPS Press branch in uistate.cpp) could
  // read as already-armed for a gesture that has not earned it. Here the next
  // press opens with no stop set at all, so a correctly-behaved widget must
  // report a flat bar throughout.
  Rig r;
  r.click(UiKey::Stops, kLeftOnly);
  ASSERT_EQ(UiFocus::Stops, r.focus());
  EXPECT_EQ(UiIntent::SetRightStop, r.click(UiKey::Right, kLeftOnly));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  // Both stops are now set (left from setup, right just now), so a fresh
  // press's own Hold really would clear both - the point here is only that no
  // STALE arming survives from before the close.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateStopsAutoClose, TheRestScreenIsFullyFunctionalAfterTheAutoClose) {
  // Not just "focus() reads Jog" - the arrows must actually drive the carriage
  // again afterwards, proving the close is a real return to Jog and not a
  // stray value that happens to compare equal.
  Rig r;
  r.click(UiKey::Stops);
  ASSERT_EQ(UiIntent::SetLeftStop, r.click(UiKey::Left, kNoStops));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::JogRightStart,
            r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  EXPECT_EQ(UiIntent::JogStop,
            r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
}

// --- The confirm bar itself (rendered by lib/display, not here) -------------

TEST(UiStateConfirmBar, IsZeroWhenNothingIsHeld) {
  Rig r;
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  r.click(UiKey::Stops, kBothStops);
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateConfirmBar, FillsOverOneSecondAndSaturates) {
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  r.advance(250);
  EXPECT_EQ(250, r.ui().stopsConfirmPermille(r.now()));
  r.advance(250);
  EXPECT_EQ(500, r.ui().stopsConfirmPermille(r.now()));
  r.advance(499);
  EXPECT_EQ(999, r.ui().stopsConfirmPermille(r.now()));
  r.advance(1);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
  // Saturates rather than wrapping, so a Release that KeyArray drops cannot
  // make the bar appear to restart.
  r.advance(10000);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateConfirmBar, ReachesFullExactlyAtTheHoldTimerLength) {
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(UiState::kStopsConfirmMs - 1);
  EXPECT_LT(r.ui().stopsConfirmPermille(r.now()), 1000);
  r.advance(1);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateConfirmBar, ReleaseCancelsItCleanly) {
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(700);
  ASSERT_EQ(700, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  // And a Hold arriving after the release - a malformed stream - must not
  // resurrect the cancelled gesture.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
}

TEST(UiStateConfirmBar, EmptiesWhenTheHoldFires) {
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(UiState::kStopsConfirmMs);
  ASSERT_EQ(UiIntent::ClearBothStops,
            r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateConfirmBar, HoldFiresOnlyOnce) {
  // A repeated Hold on the same press must not clear twice.
  Rig r;
  r.click(UiKey::Stops, kBothStops);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  ASSERT_EQ(UiIntent::ClearBothStops,
            r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
}

TEST(UiStateConfirmBar, DoesNotFillForAGestureThatWouldBeRefused) {
  // A bar that fills and then does nothing is the silent-ignore failure §4 and
  // menuTileBlock() both exist to prevent. It arms only when the hold would
  // actually succeed.
  Rig underPower;
  UiContext moving = ctx(true, true, /*motionEnabled=*/true);
  underPower.click(UiKey::Stops, moving);
  underPower.key(UiKey::Stops, UiKeyEvent::Press, moving);
  underPower.advance(500);
  EXPECT_EQ(0, underPower.ui().stopsConfirmPermille(underPower.now()));

  Rig noStops;
  noStops.click(UiKey::Stops, kNoStops);
  noStops.key(UiKey::Stops, UiKeyEvent::Press, kNoStops);
  noStops.advance(500);
  EXPECT_EQ(0, noStops.ui().stopsConfirmPermille(noStops.now()));

  Rig menuOpen;  // the one focus the press does not reach out of
  menuOpen.click(UiKey::Menu, kBothStops);
  ASSERT_TRUE(menuOpen.ui().menuOpen());
  menuOpen.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  menuOpen.advance(500);
  EXPECT_EQ(0, menuOpen.ui().stopsConfirmPermille(menuOpen.now()));
}

TEST(UiStateConfirmBar, FillsWhileHeldFromTheRestScreen) {
  // The point of the owner's ruling: the bar has to be VISIBLE while it fills,
  // and from the rest screen it now is - the Press opens the widget, so the
  // operator watches the second run out over the stop markers and the travel
  // bar they are about to lose. (Before, a hold from rest armed nothing and
  // showed nothing.)
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  ASSERT_EQ(UiFocus::Stops, r.focus());  // the widget the bar is drawn over
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  r.advance(300);
  EXPECT_EQ(300, r.ui().stopsConfirmPermille(r.now()));
  r.advance(400);
  EXPECT_EQ(700, r.ui().stopsConfirmPermille(r.now()));
  r.advance(UiState::kStopsConfirmMs - 700);
  EXPECT_EQ(1000, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::ClearBothStops,
            r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
}

TEST(UiStateConfirmBar, DoesNotFillFromTheRestScreenForAGestureThatWouldFail) {
  // ...and it still arms only when the gesture would succeed. Opening the
  // widget on the Press must not arm a bar that is going to be refused.
  Rig noStops;
  noStops.key(UiKey::Stops, UiKeyEvent::Press, kNoStops);
  noStops.advance(500);
  EXPECT_EQ(0, noStops.ui().stopsConfirmPermille(noStops.now()));

  Rig underPower;
  UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                         /*motionActive=*/true);
  underPower.key(UiKey::Stops, UiKeyEvent::Press, moving);
  underPower.advance(500);
  EXPECT_EQ(0, underPower.ui().stopsConfirmPermille(underPower.now()));
}

TEST(UiStateConfirmBar, ReleaseFromTheRestScreenCancelsButLeavesTheWidgetOpen) {
  // Letting go early is the escape hatch: the bar goes, nothing is cleared, and
  // the widget the Press opened stays up - the operator is now simply in the
  // STOPS widget, exactly as a tap would have left them.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(600);
  ASSERT_EQ(600, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Stops, UiKeyEvent::Release, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiFocus::Stops, r.focus());
}

TEST(UiStateConfirmBar, HaltDuringARestScreenHoldCancelsEverything) {
  // HALT outranks the new gesture like it outranks every other one: the bar
  // goes, focus returns to Jog, and the Hold that arrives afterwards - the
  // operator has not let go yet - must not clear anything. Also the self-heal
  // path if the STOPS Release is ever dropped: any event on any other key ends
  // the confirm.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(500);
  ASSERT_EQ(500, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::CancelMotion,
            r.key(UiKey::Halt, UiKeyEvent::Press, kBothStops));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, kBothStops));
}

TEST(UiStateConfirmBar, IsCancelledByAnyOtherKeyEvent) {
  // The self-healing path for a STOPS Release that KeyArray drops (its 10 ms
  // debounce and its one-second matrix rescan can both swallow one). Without
  // this the bar would sit pinned on screen for ever.
  const UiKey others[] = {UiKey::Halt, UiKey::Menu, UiKey::Left, UiKey::Ok,
                          UiKey::Enable, UiKey::EncoderCw};
  for (UiKey k : others) {
    Rig r;
    r.click(UiKey::Stops, kBothStops);
    r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
    r.advance(400);
    ASSERT_EQ(400, r.ui().stopsConfirmPermille(r.now()));
    r.key(k, UiKeyEvent::Press, kBothStops);
    EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
  }
}

TEST(UiStateConfirmBar, SurvivesMillisRollover) {
  // nowMs is millis(), which wraps at ~49.7 days. The elapsed calculation must
  // be the unsigned-difference form, or a press taken just before the wrap
  // reads as ~49 days old and the bar snaps to full instantly. Driven directly
  // rather than through Rig so the clock can be placed at the top of its range,
  // whatever width unsigned long has on this host.
  const unsigned long kNearMax = ~0UL - 200UL;
  UiState ui;
  const UiContext c = kBothStops;
  ui.handleKey(UiKey::Stops, UiKeyEvent::Press, c, kNearMax);
  ui.handleKey(UiKey::Stops, UiKeyEvent::Click, c, kNearMax);
  ui.handleKey(UiKey::Stops, UiKeyEvent::Release, c, kNearMax);
  ASSERT_EQ(UiFocus::Stops, ui.focus());

  ui.handleKey(UiKey::Stops, UiKeyEvent::Press, c, kNearMax);
  const unsigned long after = kNearMax + 512UL;  // wraps
  ASSERT_LT(after, kNearMax) << "the test clock must actually have wrapped";
  EXPECT_EQ(512, ui.stopsConfirmPermille(after));
}

// ===========================================================================
// 12. ENABLE: first press dismisses, second engages (spec §5, refined)
//
// §5's "toggle MM_ENABLED <-> MM_DECELLERATE" is unchanged and still lives in
// src/buttonpad.cpp; what moved here is WHETHER the toggle is reached.
// Engaging is a commitment to cut, and it must not happen while the operator's
// attention is still inside a picker.
// ===========================================================================

TEST(UiStateEnable, AtRestReturnsToggleEngage) {
  Rig r;
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateEnable, WithAWidgetOpenOnlyDismisses) {
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops};
  for (UiFocus f : widgets) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable)) << "focus=" << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "focus=" << f;
  }
}

TEST(UiStateEnable, WithTheMenuOpenOnlyDismisses) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateEnable, SecondPressEngages) {
  // The whole point of the rule: it costs one extra press, never a lost one.
  const UiFocus openThings[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                                UiFocus::Stops, UiFocus::Menu};
  for (UiFocus f : openThings) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(UiIntent::None, r.click(UiKey::Enable)) << "focus=" << f;
    EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable)) << "focus=" << f;
  }
}

TEST(UiStateEnable, PressReleaseAndHoldAreInert) {
  // The old handler acted on BS_CLICKED alone; that is preserved. Press or
  // Release acting too would toggle twice across a single tap.
  const UiKeyEvent events[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                               UiKeyEvent::Hold};
  for (UiFocus f : kAllFocuses) {
    for (UiKeyEvent ev : events) {
      Rig r;
      r.enterFocus(f);
      EXPECT_EQ(UiIntent::None, r.key(UiKey::Enable, ev, kNoStops))
          << "focus=" << f << " event=" << ev;
    }
  }
}

TEST(UiStateEnable, HoldDoesNotDismissEither) {
  // Only a Click dismisses, so a slow press inside a widget leaves it open
  // rather than closing it a second later under the operator's fingers.
  Rig r;
  r.click(UiKey::Rate);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Enable));
  EXPECT_EQ(UiFocus::Rate, r.focus());
}

TEST(UiStateEnable, DisengagingFromRestStillWorks) {
  // ToggleEngage is symmetric - ButtonPad decides which way it goes from the
  // motion mode - so the disengage path must reach it too.
  Rig r;
  UiContext c = ctx(false, false, /*motionEnabled=*/true,
                    /*motionActive=*/true);
  EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable, c));
}

TEST(UiStateEnable, DismissDoesNotCancelMotion) {
  // ENABLE closing a widget is a UI action only. HALT is the key that stops the
  // machine, and it is unconditional (§5); ENABLE must not quietly do it too,
  // or dismissing a picker would abort a cut.
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  const UiContext running = ctx(true, false, false, /*motionActive=*/true);
  // The widget is opened from the COMMANDED window - the run has been ordered
  // but no context has reported it yet, so the machine still reads as at rest
  // and RATE is still live. That is the only panel-reachable route to a widget
  // over a live run since the motion lockout, and it is a real one: the caller
  // rebuilds the context from the machine, which cannot have moved yet.
  r.click(UiKey::Rate, kLeftOnly);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable, running));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  // The run latch survived the dismiss, so the arrows still cancel it.
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, running));
}

TEST(UiStateEnable, DoesNotStrandAnInFlightJog) {
  // A dead-man jog is stopped by the Release of its own arrow, and nothing
  // about ENABLE may interfere with that.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart,
            r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  r.click(UiKey::Enable);
  EXPECT_EQ(UiIntent::JogStop,
            r.key(UiKey::Left, UiKeyEvent::Release, kNoStops));
}

// ===========================================================================
// 12. Where OK on a tile lands you (the owner's rule for this feature set)
//
// "OK always closes the menu, and you always land somewhere that shows the
// result." The main screen IS the confirmation for the tiles whose effect is
// visible there; the rest open a screen of their own. The ONE exception is a
// refused tile, which changes nothing and stays open with its reason.
//
// These are the contract for menuTileDestination() as UiState applies it, so
// the table is asserted tile by tile rather than through the helper - a test
// that asked menuTileDestination() what it expected would pass no matter what
// the table said.
// ===========================================================================

TEST(UiStateMenuDestination, UnitsLandsOnTheMainScreen) {
  // The pitch redraws in the new unit; that redraw is the feedback.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_UNITS));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuDestination, ThemeLandsOnTheMainScreen) {
  // The whole screen changes colour.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_THEME));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuDestination, DroDatumOpensItsOwnOverlay) {
  // Not visible on the main screen at the moment of the press, so it gets a
  // picker rather than a silent toggle behind the carousel.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_DRO_DATUM));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::DroDatum, r.focus());
}

TEST(UiStateMenuDestination, JogSpeedOpensTheExistingWidget) {
  // The same widget OK opens at rest (§6, "here for discoverability"). This is
  // what replaced ButtonPad replaying two synthetic keystrokes to move focus.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_JOG_SPEED));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::JogSpeed, r.focus());
}

TEST(UiStateMenuDestination, SyncLandsOnTheMainScreen) {
  // The sync indicator lights in the status bar. Needs a thread mode, or the
  // tile is refused - which is the next section.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_SYNC, kThreadIdle));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuDestination, SoftwareUpdateLandsOnTheMainScreen) {
  // The OTA screen takes over from GlobalState::hasOTA(), not from a UiFocus,
  // so Jog is the correct focus to leave behind it.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_SOFTWARE_UPDATE));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuDestination, WifiSetupLandsOnTheMainScreen) {
  // Academic - ButtonPad reboots - but the state must still be coherent for the
  // instant before it goes, and for the case where the reboot is refused.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_WIFI_SETUP));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuDestination, DiagnosticsOpensItsOwnScreen) {
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_DIAGNOSTICS));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Diagnostics, r.focus());
}

TEST(UiStateMenuDestination, AboutOpensItsOwnScreen) {
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_ABOUT));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::About, r.focus());
}

TEST(UiStateMenuDestination, EveryTileClosesTheMenuAndLandsSomewhere) {
  // The sweep, so a tile added later cannot quietly inherit the old "stays
  // open" behaviour by being left out of the table above.
  for (int tile = 0; tile < kMenuItems; ++tile) {
    Rig r;
    EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(tile))
        << "tile " << tile;
    EXPECT_FALSE(r.ui().menuOpen()) << "tile " << tile;
    EXPECT_NE(UiFocus::Menu, r.focus()) << "tile " << tile;
  }
}

// ---------------------------------------------------------------------------
// THE EXCEPTION: a refused tile changes nothing and stays open.
//
// Not decoration. The reason the tile was refused is ALREADY on screen - the
// display evaluates the same menuTileBlock() every tick, so the tile is drawn
// dim and the hint row reads "stop the carriage first" / "needs thread mode".
// Closing on a refusal would throw that explanation away at the exact moment
// the operator needs to read it, and drop them on a rest screen where nothing
// happened for no visible reason.
//
// MUTATION-VERIFIED: deleting the `!= MTB_NONE` early return in uistate.cpp
// (so a refused tile closes and activates like any other) fails
// RefusedTileStaysOpenAndEmitsNothing, RefusedByFeedModeAlsoStaysOpen,
// EveryBlockableTileStaysOpenUnderPower and TheRefusalIsNotJustASuppressedIntent.
// ---------------------------------------------------------------------------

TEST(UiStateMenuRefusal, RefusedTileStaysOpenAndEmitsNothing) {
  // Theme under power: blocked by MTB_MOTION, because persisting it is a flash
  // erase that stalls the spindle loop.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  // Opened and walked AT REST - since the motion lockout the carousel cannot be
  // opened under power at all, so the reachable form of this case is motion
  // starting (web UI, spindle-driven feed) with the menu already up.
  r.click(UiKey::Menu, kNoStops);
  r.click(UiKey::Right, kNoStops);
  ASSERT_EQ((int)MENU_THEME, r.ui().menuIndex());

  EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok, moving))
      << "a refused tile must not fire";
  EXPECT_TRUE(r.ui().menuOpen()) << "the reason is on screen; keep it there";
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_EQ((int)MENU_THEME, r.ui().menuIndex())
      << "and the selection must not move";
}

TEST(UiStateMenuRefusal, RefusedByFeedModeAlsoStaysOpen) {
  // Sync at rest but in a feed mode: MTB_FEED_MODE. The other half of the rule,
  // and the one that proves the refusal is not just "is the machine moving".
  Rig r;
  r.click(UiKey::Menu, kNoStops);  // kNoStops is a FEED mode
  for (int i = 0; i < (int)MENU_SYNC; ++i) {
    r.click(UiKey::Right, kNoStops);
  }
  ASSERT_EQ((int)MENU_SYNC, r.ui().menuIndex());

  EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok, kNoStops));
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Menu, r.focus());
}

TEST(UiStateMenuRefusal, SyncFiresOnceTheModeIsAThreadMode) {
  // The same tile, the same rest state, one flag different - so the refusal
  // above is attributable to threadMode and nothing else.
  Rig r;
  EXPECT_EQ(UiIntent::MenuActivate, r.activateTile(MENU_SYNC, kThreadIdle));
  EXPECT_FALSE(r.ui().menuOpen());
}

TEST(UiStateMenuRefusal, EveryBlockableTileStaysOpenUnderPower) {
  // The four tiles menuTileBlock() gates on motion, plus Sync, which is gated
  // on motion first. Under power every one of them must be inert.
  const MenuTile blockable[] = {MENU_THEME, MENU_DRO_DATUM, MENU_SYNC,
                                MENU_SOFTWARE_UPDATE, MENU_WIFI_SETUP};
  for (MenuTile tile : blockable) {
    // motionActive AND threadMode: the thread mode removes the other reason
    // Sync could be refused, so motion is provably the cause for all five.
    const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                                 /*motionActive=*/true, /*threadMode=*/true);
    Rig r;
    r.click(UiKey::Menu, kThreadIdle);  // opened at rest - see the note above
    for (int i = 0; i < (int)tile; ++i) {
      r.click(UiKey::Right, kThreadIdle);
    }
    ASSERT_EQ((int)tile, r.ui().menuIndex());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok, moving)) << "tile " << (int)tile;
    EXPECT_TRUE(r.ui().menuOpen()) << "tile " << (int)tile;
    EXPECT_EQ(UiFocus::Menu, r.focus()) << "tile " << (int)tile;
  }
}

TEST(UiStateMenuRefusal, NoTileFiresUnderPowerNotEvenAnUnblockedOne) {
  // CHANGED BY THE MOTION LOCKOUT. This used to be
  // UnblockedTilesStillFireUnderPower, and it pinned the OPPOSITE: that the
  // refusal was per-tile and not "a blanket no menu while moving", so Units,
  // Jog speed, Diagnostics and About stayed live throughout. The owner's ruling
  // makes it a blanket rule on purpose - "it shouldn't be possible to open a
  // menu or tile while moving" - so OK is inert on every tile, including the
  // four menuTileBlock() still calls MTB_NONE.
  //
  // menuTileBlock() itself is UNCHANGED and still per-tile: it is what the
  // display dims with, and what ButtonPad re-checks against the machine. This
  // test is about the panel, not about that rule.
  const MenuTile live[] = {MENU_UNITS, MENU_JOG_SPEED, MENU_DIAGNOSTICS,
                           MENU_ABOUT};
  for (MenuTile tile : live) {
    const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                                 /*motionActive=*/true);
    ASSERT_EQ(MTB_NONE, menuTileBlock((int)tile, /*motionActive=*/true,
                                      /*threadMode=*/false))
        << "tile " << (int)tile << " must be one menuTileBlock() allows, or "
        << "this test is not about the lockout";
    Rig r;
    r.click(UiKey::Menu, kNoStops);  // opened at rest; motion starts under it
    for (int i = 0; i < (int)tile; ++i) {
      r.click(UiKey::Right, kNoStops);
    }
    ASSERT_EQ((int)tile, r.ui().menuIndex());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok, moving))
        << "tile " << (int)tile;
    EXPECT_TRUE(r.ui().menuOpen()) << "tile " << (int)tile;
    EXPECT_EQ(UiFocus::Menu, r.focus()) << "tile " << (int)tile;
    // ...and the same press fires the instant the carriage stops.
    EXPECT_EQ(UiIntent::MenuActivate, r.click(UiKey::Ok, kNoStops))
        << "tile " << (int)tile;
    EXPECT_EQ(menuTileDestination((int)tile), r.focus()) << "tile " << (int)tile;
  }
}

TEST(UiStateMenuRefusal, TheRefusalIsNotJustASuppressedIntent) {
  // The failure mode this pins: closing the carousel and moving focus, and only
  // then swallowing the intent. The operator would see the menu vanish with
  // nothing changed and no reason left on screen - worse than the old bug,
  // because at least that one left the explanation up. So after a refusal the
  // carousel must still be usable, in place, with the selection intact.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.click(UiKey::Menu, kNoStops);   // opened at rest - see the note above
  r.click(UiKey::Right, kNoStops);  // -> Theme, which the refusal will block
  ASSERT_EQ(UiIntent::None, r.click(UiKey::Ok, moving));

  // The carousel is intact: still open, still on the same tile. (Under power
  // the arrows are inert too now, so it is checked once the carriage stops -
  // which is also when the operator could act on it.)
  EXPECT_TRUE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_EQ((int)MENU_THEME, r.ui().menuIndex());

  // Still a live carousel: the arrows walk it again at rest.
  EXPECT_EQ(UiIntent::MenuNext, r.click(UiKey::Right, kNoStops));
  EXPECT_EQ((int)MENU_DRO_DATUM, r.ui().menuIndex());
  EXPECT_TRUE(r.ui().menuOpen());
  // And MENU still closes it the ordinary way.
  EXPECT_EQ(UiIntent::CloseMenu, r.click(UiKey::Menu, kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMenuRefusal, ARefusedTileFiresOnceTheCarriageStops) {
  // The refusal is a state, not a latch: nothing is remembered about it, so the
  // same press works the moment the machine is at rest.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.click(UiKey::Menu, kNoStops);   // opened at rest - see the note above
  r.click(UiKey::Right, kNoStops);
  ASSERT_EQ(UiIntent::None, r.click(UiKey::Ok, moving));
  EXPECT_EQ(UiIntent::MenuActivate, r.click(UiKey::Ok, kNoStops));
  EXPECT_FALSE(r.ui().menuOpen());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

// ===========================================================================
// 13. The DRO datum overlay (UiFocus::DroDatum)
//
// Modelled on Mode: a small set of choices, arrows pick, OK commits and
// dismisses, HALT cancels, 4 s idle drops back to Jog. The intents are ABSOLUTE
// (DroDatumLeft / DroDatumRight) rather than a next/prev pair, because with two
// choices a next/prev pair is a toggle and a toggle cannot be pressed twice
// safely - see the arrow branch in uistate.cpp.
// ===========================================================================

TEST(UiStateDroDatum, ArrowsPickAnEndAbsolutely) {
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  ASSERT_EQ(UiFocus::DroDatum, r.focus());
  EXPECT_EQ(UiIntent::DroDatumLeft, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::DroDatumRight, r.click(UiKey::Right));
  EXPECT_EQ(UiFocus::DroDatum, r.focus()) << "picking must not dismiss";
}

TEST(UiStateDroDatum, RepeatingAnArrowNamesTheSameEnd) {
  // THE reason the intents are absolute. A next/prev toggle would return
  // DroDatumRight on the second press and silently undo the first - which is
  // exactly the "did that register?" failure this feature set exists to kill.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::DroDatumLeft, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::DroDatumLeft, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::DroDatumLeft, r.click(UiKey::Left));
  EXPECT_EQ(UiIntent::DroDatumRight, r.click(UiKey::Right));
  EXPECT_EQ(UiIntent::DroDatumRight, r.click(UiKey::Right));
}

TEST(UiStateDroDatum, ArrowsIgnoreEveryEventThatIsNotAClick) {
  // Press/Release inert or one tap acts twice; Hold has no meaning here.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  const UiKeyEvent inert[] = {UiKeyEvent::Press, UiKeyEvent::Release,
                              UiKeyEvent::Hold};
  for (UiKeyEvent ev : inert) {
    EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, ev)) << ev;
    EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, ev)) << ev;
    EXPECT_EQ(UiFocus::DroDatum, r.focus()) << ev;
  }
}

TEST(UiStateDroDatum, OkCommitsAndReturnsToJog) {
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  r.click(UiKey::Right);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateDroDatum, OkHoldInsideTheOverlayDoesNotZeroTheDro) {
  // Same guard every other widget has: ZeroDro is defined at rest only, so a
  // slow OK press inside a picker can never move the datum by accident.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::None, r.hold(UiKey::Ok));
}

TEST(UiStateDroDatum, HaltCancelsBackToJog) {
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_FALSE(r.ui().menuOpen());
}

TEST(UiStateDroDatum, ExpiresAfterTheIdleTimeout) {
  // It IS a widget, so the 4 s rule applies: an overlay left open over the rest
  // screen re-points the arrows at a setting the operator has forgotten about.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  ASSERT_EQ(UiFocus::DroDatum, r.focus());
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick());
  r.advance(1);
  EXPECT_TRUE(r.tick());
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateDroDatum, EnableDismissesItWithoutEngaging) {
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable));
}

TEST(UiStateDroDatum, MenuKeyOpensTheCarouselOverIt) {
  // It is a widget, not a read-only screen, so MENU behaves as it does over
  // every other widget: top level, opens on top.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu));
  EXPECT_EQ(UiFocus::Menu, r.focus());
  EXPECT_TRUE(r.ui().menuOpen());
}

TEST(UiStateDroDatum, TheKnobPicksAnEndToo) {
  // The knob drives what the focus owns, exactly like Mode. Unlike STOPS, the
  // choice is instantly reversible, so there is no reason to make it inert.
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::DroDatumRight,
            r.key(UiKey::EncoderCw, UiKeyEvent::Click));
  EXPECT_EQ(UiIntent::DroDatumLeft,
            r.key(UiKey::EncoderCcw, UiKeyEvent::Click));
  EXPECT_EQ(UiFocus::DroDatum, r.focus());
}

TEST(UiStateDroDatum, ArrowsAreInhibitedUnderPower) {
  // Unlike Rate and Mode, whose arrows must keep working mid-cut (§3), this
  // widget's arrows go dead under power - persisting the datum is a flash erase
  // that saveLathePreferences() refuses outright while the carriage is moving,
  // so emitting the intent would guarantee a silent no-op.
  for (int enabled = 0; enabled <= 1; ++enabled) {
    for (int active = 0; active <= 1; ++active) {
      if (!enabled && !active) {
        continue;  // at rest; covered by ArrowsPickAnEndAbsolutely
      }
      const UiContext c = ctx(false, false, enabled != 0, active != 0);
      Rig r;
      r.enterFocus(UiFocus::DroDatum);  // opened at rest, as the tile requires
      EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, c))
          << "enabled=" << enabled << " active=" << active;
      EXPECT_EQ(UiIntent::None, r.click(UiKey::Right, c))
          << "enabled=" << enabled << " active=" << active;
      EXPECT_EQ(UiFocus::DroDatum, r.focus()) << "and the widget stays open";
    }
  }
}

TEST(UiStateDroDatum, ArrowsAreLiveAgainOnceTheCarriageStops) {
  // The inhibit is a state, not a latch.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  ASSERT_EQ(UiIntent::None, r.click(UiKey::Left, moving));
  EXPECT_EQ(UiIntent::DroDatumLeft, r.click(UiKey::Left, kNoStops));
}

TEST(UiStateDroDatum, TheKnobIsStillDeadUnderPower) {
  // The blanket encoder inhibit applies here like everywhere else.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  Rig r;
  r.enterFocus(UiFocus::DroDatum);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::EncoderCw, UiKeyEvent::Click, moving));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::EncoderCcw, UiKeyEvent::Click, moving));
}

// ===========================================================================
// 14. The read-only screens (UiFocus::Diagnostics, UiFocus::About)
//
// OK, MENU and HALT all get you out to Jog; the arrows do nothing; and - the
// deliberate ruling - the 4 s idle timeout does NOT apply, because these are
// screens you are meant to be able to watch or read for longer than that and
// they carry none of the hazard the timeout exists to guard against.
// ===========================================================================

TEST(UiStateReadOnlyScreens, OkReturnsToJog) {
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok)) << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << f;
  }
}

TEST(UiStateReadOnlyScreens, MenuReturnsToJogRatherThanReopeningTheCarousel) {
  // The key that took the operator in is the obvious one to take them back out,
  // and re-opening the carousel would put a picker on top of a screen that was
  // itself opened from that picker.
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::CloseMenu, r.click(UiKey::Menu)) << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << f;
    EXPECT_FALSE(r.ui().menuOpen()) << f;
  }
}

TEST(UiStateReadOnlyScreens, HaltReturnsToJogAndStillCancelsMotion) {
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt)) << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << f;
  }
}

TEST(UiStateReadOnlyScreens, ArrowsDoNothingAtAll) {
  // Inert on every event, in every stop configuration - LEFT and RIGHT are the
  // jog keys, so a reflex press on a read-only screen must not move metal, step
  // a value, or leave the screen.
  const UiKeyEvent every[] = {UiKeyEvent::Press, UiKeyEvent::Click,
                              UiKeyEvent::Hold, UiKeyEvent::Release};
  const UiContext contexts[] = {kNoStops, kLeftOnly, kRightOnly, kBothStops};
  for (UiFocus f : kReadOnlyScreens) {
    for (const UiContext& c : contexts) {
      for (UiKeyEvent ev : every) {
        Rig r;
        r.enterFocus(f);
        EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, ev, c)) << f << " " << ev;
        EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, ev, c)) << f << " " << ev;
        EXPECT_EQ(f, r.focus()) << f << " " << ev;
      }
    }
  }
}

TEST(UiStateReadOnlyScreens, TheKnobIsInertToo) {
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::None, r.key(UiKey::EncoderCw, UiKeyEvent::Click)) << f;
    EXPECT_EQ(UiIntent::None, r.key(UiKey::EncoderCcw, UiKeyEvent::Click)) << f;
    EXPECT_EQ(f, r.focus()) << f;
  }
}

TEST(UiStateReadOnlyScreens, DoNotTimeOut) {
  // THE RULING. Diagnostics shows following error and pulse counts, which only
  // mean anything watched over a spindle revolution or a test pass - and the
  // operator's hands are on the machine, not the panel, so there is no input to
  // keep a timer alive with. About is a screen you read an IP address off. Four
  // seconds is not enough for either, and neither carries the hazard the
  // timeout exists for (their arrows are inert - see above), so neither expires.
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus());
    r.advance(kTimeout * 100);
    EXPECT_FALSE(r.tick()) << f;
    EXPECT_EQ(f, r.focus()) << f << " must survive an arbitrary idle period";
  }
}

TEST(UiStateReadOnlyScreens, EnableDismissesAboutWithoutEngaging) {
  // Same rule as a widget: engaging is a commitment to cut, and it must not
  // happen while a full screen is hiding the rest screen and the state chip.
  //
  // NARROWED from both read-only screens to About alone. The owner overturned
  // it for Diagnostics - that screen is meant to be watched WHILE cutting, so
  // requiring a dismiss before engaging would make it impossible to have up
  // for the run it instruments. See UiStateDiagnosticsSurvivesMotion.
  Rig r;
  r.enterFocus(UiFocus::About);
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable));
}

TEST(UiStateReadOnlyScreens, OkHoldDoesNotZeroTheDro) {
  for (UiFocus f : kReadOnlyScreens) {
    Rig r;
    r.enterFocus(f);
    EXPECT_EQ(UiIntent::None, r.hold(UiKey::Ok)) << f;
  }
}

TEST(UiStateMenuOpenedFocuses, AreAllReachableAndAllLeaveOnHalt) {
  // The sweep over the three focuses only a tile can open: each is entered by a
  // real gesture sequence, and HALT - the one key that is live from everywhere -
  // gets out of all of them.
  for (UiFocus f : kMenuOpenedFocuses) {
    Rig r;
    r.enterFocus(f);
    ASSERT_EQ(f, r.focus()) << f << " is not reachable from its tile";
    ASSERT_FALSE(r.ui().menuOpen()) << f;
    EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Halt)) << f;
    EXPECT_EQ(UiFocus::Jog, r.focus()) << f;
  }
}

// ===========================================================================
// 15. THE MOTION LOCKOUT (OWNER RULING)
//
//   "I think every button except halt and enable should be disabled whilst
//    moving. So it shouldn't be possible to open a menu or tile while moving.
//    When moving, all of the operator's attention should be on the tool and
//    workpiece, not the screen/menus."
//
// One rule replacing five separate "moving, X disabled" behaviours. The arrows
// are the single refinement, and it is safety-critical: their STOPPING halves
// survive, because disabling them wholesale would delete the dead-man
// terminator and the run cancel - the two gestures whose whole job is to stop
// the machine. Stated once: while moving, the only live functions are the ones
// that stop things.
//
// The sections above pin the rule where it changed an existing behaviour. This
// section pins the rule ITSELF, exhaustively, so that removing the lockout
// fails here loudly rather than only in whatever test happened to observe it.
// ===========================================================================

namespace {

// Every context in which the lockout applies: engaged, powered-run/jog/decel,
// and both at once. Deliberately NOT including the at-rest context - the
// "everything is live again" tests below cover that side.
const UiContext kUnderPower[] = {
    ctx(true, true, /*motionEnabled=*/true, /*motionActive=*/false),
    ctx(true, true, /*motionEnabled=*/false, /*motionActive=*/true),
    ctx(true, true, /*motionEnabled=*/true, /*motionActive=*/true),
};

// The keys the ruling makes inert. HALT and ENABLE are excluded because they
// are explicitly unchanged; the arrows are excluded because they keep their
// stopping halves and get their own tests.
const UiKey kLockedOutKeys[] = {UiKey::Mode, UiKey::Rate,       UiKey::Stops,
                                UiKey::Menu, UiKey::Ok,         UiKey::EncoderCw,
                                UiKey::EncoderCcw};

const UiKeyEvent kEveryEvent[] = {UiKeyEvent::Press, UiKeyEvent::Click,
                                  UiKeyEvent::Hold, UiKeyEvent::Release};

}  // namespace

TEST(UiStateMotionLockout, EveryLockedOutKeyIsInertFromEveryFocus) {
  // The whole rule in one sweep: every locked-out key, every event, every
  // under-power context, entered from every focus an operator can be in when
  // the carriage starts moving. Nothing may be emitted and nothing may move.
  for (UiFocus f : kAllFocuses) {
    for (const UiContext& c : kUnderPower) {
      for (UiKey k : kLockedOutKeys) {
        for (UiKeyEvent ev : kEveryEvent) {
          Rig r;
          r.enterFocus(f);  // at rest, as an operator would
          ASSERT_EQ(f, r.focus()) << "setup failed for " << f;
          const bool menuWasOpen = r.ui().menuOpen();
          const int indexWas = r.ui().menuIndex();

          EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
              << "focus=" << f << " event=" << ev;
          EXPECT_EQ(f, r.focus())
              << "focus must not move under power (focus=" << f
              << " event=" << ev << ")";
          EXPECT_EQ(menuWasOpen, r.ui().menuOpen()) << "focus=" << f;
          EXPECT_EQ(indexWas, r.ui().menuIndex()) << "focus=" << f;
        }
      }
    }
  }
}

TEST(UiStateMotionLockout, NoMenuOrTileCanBeOpenedWhileMoving) {
  // The owner's sentence, taken literally: "it shouldn't be possible to open a
  // menu or tile while moving." Both halves - the carousel itself, and the
  // three focuses a tile opens.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu, c));
    EXPECT_FALSE(r.ui().menuOpen()) << "the carousel must not open";
    EXPECT_EQ(UiFocus::Jog, r.focus());

    // And with the carousel already up from before the machine started, OK
    // cannot activate a tile either - so DroDatum, Diagnostics and About are
    // unreachable while moving however the operator got here.
    Rig r2;
    r2.click(UiKey::Menu, kNoStops);
    ASSERT_TRUE(r2.ui().menuOpen());
    for (int tile = 0; tile < kMenuItems; ++tile) {
      Rig r3;
      r3.click(UiKey::Menu, kNoStops);
      for (int i = 0; i < tile; ++i) {
        r3.click(UiKey::Right, kNoStops);
      }
      ASSERT_EQ(tile, r3.ui().menuIndex());
      EXPECT_EQ(UiIntent::None, r3.click(UiKey::Ok, c)) << "tile " << tile;
      EXPECT_TRUE(r3.ui().menuOpen()) << "tile " << tile;
      EXPECT_EQ(UiFocus::Menu, r3.focus()) << "tile " << tile;
    }
  }
}

TEST(UiStateMotionLockout, NoSettingCanBeAdjustedWhileMoving) {
  // The other half of "inert": not just no focus change, but no VALUE change.
  // The arrows are included here because although they keep their stopping
  // halves, they may never step a setting - so this walks them from inside each
  // widget the operator could have had open when the machine started.
  const UiFocus widgets[] = {UiFocus::JogSpeed, UiFocus::Rate, UiFocus::Mode,
                             UiFocus::Stops, UiFocus::DroDatum};
  const UiKey steppers[] = {UiKey::Left, UiKey::Right, UiKey::EncoderCw,
                            UiKey::EncoderCcw};
  for (UiFocus f : widgets) {
    for (const UiContext& c : kUnderPower) {
      for (UiKey k : steppers) {
        for (UiKeyEvent ev : kEveryEvent) {
          Rig r;
          r.enterFocus(f);
          ASSERT_EQ(f, r.focus());
          EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
              << "focus=" << f << " event=" << ev;
        }
      }
    }
  }
}

TEST(UiStateMotionLockout, TheDeadManTerminatorStillWorksUnderPower) {
  // SAFETY-CRITICAL, and the first of the two arrow functions the lockout must
  // not touch. A hold-to-jog is only safe if letting go ALWAYS stops it, so the
  // Release must emit JogStop no matter what the context says - and a jog IS
  // motion, so an under-power context is the NORMAL case for this release, not
  // an edge one. If the lockout ever swallows it, m_jogDir is stranded and the
  // carriage keeps running with no JogStop ever emitted.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    for (const UiContext& c : kUnderPower) {
      Rig r;
      ASSERT_EQ(k == UiKey::Left ? UiIntent::JogLeftStart
                                 : UiIntent::JogRightStart,
                r.key(k, UiKeyEvent::Press, kNoStops));
      EXPECT_EQ(UiIntent::JogStop, r.key(k, UiKeyEvent::Release, c))
          << "letting go must stop the carriage under power";
      // Genuinely over: the terminator does not re-fire, and a fresh press
      // starts a new jog once the machine is back at rest.
      EXPECT_EQ(UiIntent::None, r.key(k, UiKeyEvent::Release, c));
    }
  }
}

TEST(UiStateMotionLockout, TheOppositeArrowReleaseAlsoTerminatesUnderPower) {
  // The terminator is not direction-matched, and the lockout must not have
  // made it so.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    ASSERT_EQ(UiIntent::JogLeftStart,
              r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
    EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Right, UiKeyEvent::Release, c));
  }
}

TEST(UiStateMotionLockout, ARunCanStillBeCancelledUnderPower) {
  // The second arrow function the lockout must not touch. A powered run to a
  // stop is exactly why underPower() is true, so a rule that inhibited the only
  // way to abort a run whenever a run was in progress would be no rule at all.
  // Either arrow, Click or Hold (§7 - the cancel is unconditional on direction).
  const UiKey started[] = {UiKey::Left, UiKey::Right};
  const UiKey cancelled[] = {UiKey::Left, UiKey::Right};
  const UiKeyEvent acting[] = {UiKeyEvent::Click, UiKeyEvent::Hold};
  for (UiKey s : started) {
    for (UiKey k : cancelled) {
      for (UiKeyEvent ev : acting) {
        Rig r;
        const UiContext running = ctx(true, true, /*motionEnabled=*/false,
                                      /*motionActive=*/true);
        ASSERT_EQ(s == UiKey::Left ? UiIntent::RunToLeftStop
                                   : UiIntent::RunToRightStop,
                  r.click(s, kBothStops));
        // The run is now confirmed by a context that reports it moving.
        ASSERT_EQ(UiIntent::None, r.key(k, UiKeyEvent::Press, running));
        EXPECT_EQ(UiIntent::CancelMotion, r.key(k, ev, running))
            << "started=" << (s == UiKey::Left ? "L" : "R")
            << " cancelled=" << (k == UiKey::Left ? "L" : "R")
            << " event=" << ev;
        // Once, not twice: the latch is gone, so the Release is inert.
        EXPECT_EQ(UiIntent::None, r.key(k, UiKeyEvent::Release, running));
      }
    }
  }
}

TEST(UiStateMotionLockout, ArrowsMayNotStartMotionUnderPower) {
  // The other side of the arrow refinement: they stop things, they never start
  // them. With no run in flight, no arrow event under power may emit a jog or
  // a run - including the Hold and Click that would otherwise run to a stop.
  const UiKey arrows[] = {UiKey::Left, UiKey::Right};
  for (UiKey k : arrows) {
    for (const UiContext& c : kUnderPower) {
      for (UiKeyEvent ev : kEveryEvent) {
        Rig r;
        EXPECT_EQ(UiIntent::None, r.key(k, ev, c))
            << "key=" << (k == UiKey::Left ? "Left" : "Right")
            << " event=" << ev;
        EXPECT_EQ(UiFocus::Jog, r.focus());
      }
    }
  }
}

TEST(UiStateMotionLockout, HaltIsCompletelyUnaffected) {
  // "except halt and enable". HALT from every focus, every acting event, every
  // under-power context.
  const UiKeyEvent acting[] = {UiKeyEvent::Press, UiKeyEvent::Click,
                               UiKeyEvent::Hold};
  for (UiFocus f : kAllFocuses) {
    for (const UiContext& c : kUnderPower) {
      for (UiKeyEvent ev : acting) {
        Rig r;
        r.enterFocus(f);
        EXPECT_EQ(UiIntent::CancelMotion, r.key(UiKey::Halt, ev, c))
            << "focus=" << f << " event=" << ev;
        EXPECT_EQ(UiFocus::Jog, r.focus()) << "focus=" << f;
        EXPECT_FALSE(r.ui().menuOpen()) << "focus=" << f;
      }
      Rig rel;
      EXPECT_EQ(UiIntent::None, rel.key(UiKey::Halt, UiKeyEvent::Release, c));
    }
  }
}

TEST(UiStateMotionLockout, EnableIsCompletelyUnaffected) {
  // The other exception, and the one that makes the lockout survivable: ENABLE
  // must still disengage. From Jog it toggles directly; from anything else it
  // dismisses first and toggles on the second press, exactly as at rest.
  for (const UiContext& c : kUnderPower) {
    Rig jog;
    EXPECT_EQ(UiIntent::ToggleEngage, jog.click(UiKey::Enable, c));
    EXPECT_EQ(UiFocus::Jog, jog.focus());

    for (UiFocus f : kAllFocuses) {
      if (f == UiFocus::Jog) {
        continue;
      }
      Rig r;
      r.enterFocus(f);  // opened at rest; the machine then starts moving
      EXPECT_EQ(UiIntent::None, r.click(UiKey::Enable, c)) << "focus=" << f;
      EXPECT_EQ(UiFocus::Jog, r.focus()) << "focus=" << f;
      EXPECT_FALSE(r.ui().menuOpen()) << "focus=" << f;
      EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable, c))
          << "focus=" << f;
    }
  }
}

TEST(UiStateMotionLockout, EverythingIsLiveAgainOnceTheCarriageIsAtRest) {
  // THE CONTROL. Without it every test above would pass on a state machine
  // that had simply been broken - the lockout must be a state, not a latch, and
  // the panel must come straight back the moment the machine stops.
  const UiContext moving = ctx(false, false, /*motionEnabled=*/true);

  Rig mode;
  ASSERT_EQ(UiIntent::None, mode.click(UiKey::Mode, moving));
  mode.click(UiKey::Mode, kNoStops);
  EXPECT_EQ(UiFocus::Mode, mode.focus());
  EXPECT_EQ(UiIntent::ModeNext, mode.click(UiKey::Right, kNoStops));

  Rig rate;
  ASSERT_EQ(UiIntent::None, rate.click(UiKey::Rate, moving));
  rate.click(UiKey::Rate, kNoStops);
  EXPECT_EQ(UiFocus::Rate, rate.focus());
  EXPECT_EQ(UiIntent::PitchNext, rate.click(UiKey::Right, kNoStops));

  Rig stops;
  ASSERT_EQ(UiIntent::None, stops.click(UiKey::Stops, moving));
  stops.click(UiKey::Stops, kNoStops);
  EXPECT_EQ(UiFocus::Stops, stops.focus());
  EXPECT_EQ(UiIntent::SetLeftStop, stops.click(UiKey::Left, kNoStops));

  Rig menu;
  ASSERT_EQ(UiIntent::None, menu.click(UiKey::Menu, moving));
  menu.click(UiKey::Menu, kNoStops);
  EXPECT_TRUE(menu.ui().menuOpen());
  EXPECT_EQ(UiIntent::MenuNext, menu.click(UiKey::Right, kNoStops));

  Rig ok;
  ASSERT_EQ(UiIntent::None, ok.click(UiKey::Ok, moving));
  ok.click(UiKey::Ok, kNoStops);
  EXPECT_EQ(UiFocus::JogSpeed, ok.focus());

  Rig zero;
  ASSERT_EQ(UiIntent::None, zero.hold(UiKey::Ok, moving));
  EXPECT_EQ(UiIntent::ZeroDro, zero.hold(UiKey::Ok, kNoStops));

  Rig knob;
  ASSERT_EQ(UiIntent::None,
            knob.key(UiKey::EncoderCw, UiKeyEvent::Click, moving));
  EXPECT_EQ(UiIntent::PitchNext,
            knob.key(UiKey::EncoderCw, UiKeyEvent::Click, kNoStops));

  Rig jog;
  ASSERT_EQ(UiIntent::None, jog.key(UiKey::Left, UiKeyEvent::Press, moving));
  EXPECT_EQ(UiIntent::JogLeftStart,
            jog.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
}

TEST(UiStateMotionLockout, AnInertKeyStillReconcilesACompletedRun) {
  // The lockout sits BELOW the run-phase reconciliation, and this is why. A run
  // ends by itself at the stop and nothing tells UiState; the latch is retired
  // by whatever key event next sees the machine at rest. If the lockout were
  // moved above the reconciliation, every keypress made while the carriage was
  // running would stop reconciling, and the "eaten click" the latch exists to
  // prevent would come back.
  Rig r;
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  const UiContext idle = ctx(true, false, /*motionEnabled=*/false,
                             /*motionActive=*/false);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));
  // A locked-out key pressed WHILE the run is live: inert, but it must still
  // hold the latch at Confirmed...
  ASSERT_EQ(UiIntent::None, r.key(UiKey::Mode, UiKeyEvent::Click, running));
  // ...and one pressed after the run has finished must retire it. (A Mode
  // PRESS, because at rest MODE is live again and its Click would take focus -
  // the event has to stay inert for the observation below to be about the
  // latch and nothing else.)
  ASSERT_EQ(UiIntent::None, r.key(UiKey::Mode, UiKeyEvent::Press, idle));
  ASSERT_EQ(UiFocus::Jog, r.focus());
  // Something is moving again by the time the operator reaches for an arrow.
  // A latch left standing would answer CancelMotion and eat this click.
  EXPECT_EQ(UiIntent::None, r.click(UiKey::Left, running));
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

// ---------------------------------------------------------------------------
// CLOSE-ON-MOTION (OWNER RULING): "any open widgets, if they can survive into
// motion, should probably be closed on motion."
//
// Deliberately NOT justified by reachability. The lockout already makes an open
// widget inert under power and no panel gesture can open one there, so this is
// belt to the lockout's braces - but a reachability argument is only true until
// someone adds a path, and it fails silently when they do. A picker on screen
// over a moving carriage is made impossible by construction instead.
//
// It lives in tick(), which now takes a UiContext, because motion very often
// starts with NO key event: the web UI, a spindle-driven feed, the natural end
// of a run. tick() is the only call that happens every display pass regardless.
// ---------------------------------------------------------------------------

TEST(UiStateCloseOnMotion, EveryOpenFocusClosesToJogWhenMotionStarts) {
  // Every focus that is not already Jog, including the carousel and the two
  // read-only screens that are otherwise exempt from every automatic dismissal.
  // Diagnostics is DELIBERATELY absent - see UiStateDiagnosticsSurvivesMotion.
  const UiFocus opened[] = {UiFocus::JogSpeed, UiFocus::Rate,
                            UiFocus::Mode,     UiFocus::Stops,
                            UiFocus::Menu,     UiFocus::DroDatum,
                            UiFocus::About};
  for (UiFocus f : opened) {
    for (const UiContext& c : kUnderPower) {
      Rig r;
      r.enterFocus(f);
      ASSERT_EQ(f, r.focus()) << "setup failed for " << f;
      EXPECT_TRUE(r.tick(c)) << f << " must close on motion";
      EXPECT_EQ(UiFocus::Jog, r.focus()) << f;
      EXPECT_FALSE(r.ui().menuOpen()) << f;
    }
  }
}

// ---------------------------------------------------------------------------
// DIAGNOSTICS SURVIVES MOTION (owner ruling).
//
//   "the diagnostic screen should survive Enable and Jog, rather than
//    dismissing like the others. It could be genuinely useful to have that on
//    screen when debugging. Ok should clear it."
//
// It is the one screen whose whole value is being watched WHILE the machine
// runs - following error and pulse counts are all zero at rest. About is not
// included: it is a static version page with no reason to be up under power.
// ---------------------------------------------------------------------------

TEST(UiStateDiagnosticsSurvivesMotion, TickLeavesItOpenUnderPower) {
  for (const UiContext& c : kUnderPower) {
    Rig r;
    r.enterFocus(UiFocus::Diagnostics);
    ASSERT_EQ(UiFocus::Diagnostics, r.focus());
    EXPECT_FALSE(r.tick(c)) << "no transition, so nothing to redraw";
    EXPECT_EQ(UiFocus::Diagnostics, r.focus()) << "must stay up while moving";
    // And it keeps surviving, tick after tick.
    EXPECT_FALSE(r.tick(c));
    EXPECT_EQ(UiFocus::Diagnostics, r.focus());
  }
}

TEST(UiStateDiagnosticsSurvivesMotion, EnableEngagesInsteadOfDismissing) {
  // Every other focus makes ENABLE a dismiss. Here it must do its real job,
  // or the operator cannot start the very motion they are watching.
  Rig r;
  r.enterFocus(UiFocus::Diagnostics);
  EXPECT_EQ(UiIntent::ToggleEngage, r.click(UiKey::Enable));
  EXPECT_EQ(UiFocus::Diagnostics, r.focus()) << "and it stays on screen";
}

TEST(UiStateDiagnosticsSurvivesMotion, AboutIsUnchanged) {
  // The ruling is about Diagnostics only.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    r.enterFocus(UiFocus::About);
    EXPECT_TRUE(r.tick(c)) << "About still closes on motion";
    EXPECT_EQ(UiFocus::Jog, r.focus());
  }
  Rig r2;
  r2.enterFocus(UiFocus::About);
  EXPECT_EQ(UiIntent::None, r2.click(UiKey::Enable)) << "About still dismisses";
  EXPECT_EQ(UiFocus::Jog, r2.focus());
}

TEST(UiStateDiagnosticsSurvivesMotion, OkClearsItAtRest) {
  Rig r;
  r.enterFocus(UiFocus::Diagnostics);
  r.click(UiKey::Ok);
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateDiagnosticsSurvivesMotion, OkClearsItUNDERPOWERToo) {
  // The motion lockout makes the whole panel inert except HALT and ENABLE.
  // OK must be exempt HERE, or "OK clears it" is false in exactly the state
  // the screen is designed to be used in. Closing a read-only screen cannot
  // start, stop or alter motion - it is the safest key on the panel.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    r.enterFocus(UiFocus::Diagnostics);
    r.click(UiKey::Ok, c);
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "OK must still clear it while moving";
  }
}

TEST(UiStateDiagnosticsSurvivesMotion, TheLockoutIsOtherwiseUntouched) {
  // Exempting OK on this one screen must not re-open the panel generally.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    r.enterFocus(UiFocus::Diagnostics);
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Menu, c));
    EXPECT_EQ(UiFocus::Diagnostics, r.focus()) << "MENU stays inert under power";
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Mode, c));
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Rate, c));
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Stops, c));
    EXPECT_EQ(UiFocus::Diagnostics, r.focus());
  }
}

TEST(UiStateDiagnosticsSurvivesMotion, OkIsStillInertOnOtherFocusesUnderPower) {
  // The exemption is keyed on the focus, not on the key.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    ASSERT_EQ(UiFocus::Jog, r.focus());
    EXPECT_EQ(UiIntent::None, r.click(UiKey::Ok, c));
    EXPECT_EQ(UiFocus::Jog, r.focus()) << "OK must not open JogSpeed under power";
  }
}

TEST(UiStateCloseOnMotion, ClosesImmediatelyNotAfterTheIdleTimeout) {
  // The idle timeout is not what does this, and must not be what does this: the
  // read-only screens and the carousel never expire, and four seconds of a
  // picker over a moving carriage is four seconds too many.
  Rig r;
  r.enterFocus(UiFocus::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  const UiContext moving = ctx(false, false, /*motionEnabled=*/true);
  EXPECT_TRUE(r.tick(moving)) << "no clock advance at all";
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_FALSE(r.ui().menuOpen());
}

TEST(UiStateCloseOnMotion, ReportsTheChangeOnlyOnce) {
  // tick()'s contract: true is a redraw TRIGGER, so it means "focus changed on
  // this call", not "focus is wrong". A second tick under the same motion has
  // nothing left to close.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    r.enterFocus(UiFocus::Rate);
    ASSERT_TRUE(r.tick(c));
    EXPECT_FALSE(r.tick(c)) << "already closed - no second redraw";
    EXPECT_FALSE(r.tick(c));
    EXPECT_EQ(UiFocus::Jog, r.focus());
  }
}

TEST(UiStateCloseOnMotion, IsInertWhenNothingIsOpen) {
  // The common case by far: at Jog with the carriage running, every pass of the
  // display loop calls tick() and it must report no change.
  for (const UiContext& c : kUnderPower) {
    Rig r;
    ASSERT_EQ(UiFocus::Jog, r.focus());
    EXPECT_FALSE(r.tick(c));
    r.advance(kTimeout * 10);
    EXPECT_FALSE(r.tick(c));
    EXPECT_EQ(UiFocus::Jog, r.focus());
  }
}

TEST(UiStateCloseOnMotion, DoesNotFireAtRest) {
  // The control. Without it this whole block would pass on a tick() that had
  // simply been made to close everything unconditionally - which would break
  // the read-only screens' exemption and every widget the operator is using.
  const UiFocus opened[] = {UiFocus::Rate, UiFocus::Menu, UiFocus::Diagnostics};
  for (UiFocus f : opened) {
    Rig r;
    r.enterFocus(f);
    EXPECT_FALSE(r.tick(kNoStops)) << f;
    EXPECT_EQ(f, r.focus()) << f << " must survive a tick at rest";
  }
}

TEST(UiStateCloseOnMotion, DoesNotDisturbAnInFlightJog) {
  // A dead-man jog IS motion, so this branch runs on every display pass for the
  // whole duration of one. It must not touch m_jogDir: the Release still has to
  // emit JogStop afterwards, or the carriage runs on with nothing to stop it.
  Rig r;
  ASSERT_EQ(UiIntent::JogLeftStart,
            r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
  const UiContext jogging = ctx(false, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  r.tick(jogging);
  r.tick(jogging);
  r.tick(jogging);
  EXPECT_EQ(UiIntent::JogStop, r.key(UiKey::Left, UiKeyEvent::Release, jogging));
}

TEST(UiStateCloseOnMotion, DoesNotDisturbAPoweredRun) {
  // Same for the run latch: a run to a stop keeps the context powered for its
  // whole duration, and the arrows must still cancel it afterwards.
  Rig r;
  ASSERT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, kLeftOnly));
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  r.tick(running);
  r.tick(running);
  EXPECT_EQ(UiIntent::CancelMotion, r.click(UiKey::Left, running));
}

TEST(UiStateCloseOnMotion, TakesTheConfirmBarWithIt) {
  // The bar is drawn over the STOPS widget, so closing the widget must empty
  // it - a full bar floating over the rest screen would promise a clear that
  // cannot happen.
  Rig r;
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(600);
  ASSERT_EQ(600, r.ui().stopsConfirmPermille(r.now()));
  const UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  EXPECT_TRUE(r.tick(moving));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}

TEST(UiStateCloseOnMotion, TheIdleTimeoutStillWorksAfterwards) {
  // The two reasons tick() can move focus must not have been collapsed into
  // one: with the carriage back at rest, the ordinary 4 s expiry is unchanged.
  Rig r;
  const UiContext moving = ctx(false, false, /*motionEnabled=*/true);
  r.enterFocus(UiFocus::Rate);
  ASSERT_TRUE(r.tick(moving));
  ASSERT_EQ(UiFocus::Jog, r.focus());

  r.click(UiKey::Rate, kNoStops);
  ASSERT_EQ(UiFocus::Rate, r.focus());
  r.advance(kTimeout - 1);
  EXPECT_FALSE(r.tick(kNoStops));
  r.advance(1);
  EXPECT_TRUE(r.tick(kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
}

TEST(UiStateMotionLockout, AConfirmBarIsNotLeftPinnedWhenMotionStarts) {
  // The bar is only ever drawn for a gesture that would succeed. If the machine
  // starts while a STOPS hold is in progress, the gesture is refused from that
  // instant, so the bar must empty rather than sit full on screen - the same
  // self-healing the "any other key" path gives it.
  Rig r;
  const UiContext moving = ctx(true, true, /*motionEnabled=*/false,
                               /*motionActive=*/true);
  r.key(UiKey::Stops, UiKeyEvent::Press, kBothStops);
  r.advance(400);
  ASSERT_EQ(400, r.ui().stopsConfirmPermille(r.now()));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Stops, UiKeyEvent::Hold, moving));
  EXPECT_EQ(0, r.ui().stopsConfirmPermille(r.now()));
}


// ---------------------------------------------------------------------------
// THE STEPPER ALARM MODAL (UiFocus::Alarm)
//
// The driver has faulted, the alarm task has stopped the axis and is HOLDING it
// stopped, and the panel's only remaining job is to show the modal and take the
// acknowledgement. So: focus is forced from wherever the operator was, no other
// key does anything at all (HALT included - the one place in this file where it
// is not first), OK asks for the clear but does NOT dismiss, and the modal
// comes down only when the fault itself has gone.
//
// Note how the alarm arrives in every test below: through the CONTEXT. There is
// no gesture that opens it, which is the same thing on the bench as it is on the
// machine.
// ---------------------------------------------------------------------------

// A latched fault with the machine already reported stopped - the steady state
// of an alarm, a few tens of ms after the trip.
const UiContext kAlarm = ctx(false, false, /*motionEnabled=*/false,
                             /*motionActive=*/false, /*threadMode=*/false,
                             /*alarm=*/true);
// The same fault while the carriage is still reported as under power: the state
// the machine is genuinely in for the first instants of a trip, before the alarm
// task's MM_DISABLED has been read back into a context.
const UiContext kAlarmMoving = ctx(false, false, /*motionEnabled=*/true,
                                   /*motionActive=*/true, /*threadMode=*/false,
                                   /*alarm=*/true);

TEST(UiStateAlarm, TakesFocusOnTheTick) {
  Rig r;
  EXPECT_TRUE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  // Only on the transition: a modal that reported a change every 100 ms would
  // repaint the whole screen for the duration of the fault.
  EXPECT_FALSE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

TEST(UiStateAlarm, ClosesAnOpenWidget) {
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  EXPECT_TRUE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

// The carousel survives everything short of MENU or HALT. It does not survive
// this.
TEST(UiStateAlarm, ClosesTheCarousel) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_TRUE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  EXPECT_FALSE(r.ui().menuOpen());
}

// Diagnostics is the one screen that survives motion. It does not survive this
// either: the numbers on it - following error, carriage rate, sync - are all
// derived from a step count the driver stopped honouring when it faulted.
TEST(UiStateAlarm, ClosesEvenTheDiagnosticsScreen) {
  Rig r;
  r.enterFocus(UiFocus::Diagnostics);
  ASSERT_EQ(UiFocus::Diagnostics, r.focus());
  EXPECT_TRUE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

TEST(UiStateAlarm, AKeyEventTakesFocusToo) {
  // Not everything waits for a tick: a key can arrive first, and it must not be
  // judged against the focus the operator was in when the driver faulted.
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Mode, UiKeyEvent::Click, kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

TEST(UiStateAlarm, EveryKeyButOkIsInert) {
  Rig r;
  r.tick(kAlarm);
  const UiKey keys[] = {UiKey::Mode,  UiKey::Rate,   UiKey::Stops,
                        UiKey::Left,  UiKey::Right,  UiKey::Halt,
                        UiKey::Menu,  UiKey::Enable, UiKey::EncoderCw,
                        UiKey::EncoderCcw};
  const UiKeyEvent evs[] = {UiKeyEvent::Press, UiKeyEvent::Click,
                            UiKeyEvent::Hold, UiKeyEvent::Release};
  for (UiKey k : keys) {
    for (UiKeyEvent e : evs) {
      EXPECT_EQ(UiIntent::None, r.key(k, e, kAlarm))
          << k << " " << e << " acted during an alarm";
      EXPECT_EQ(UiFocus::Alarm, r.focus());
    }
  }
}

// HALT gets its own test because it is the exception to this file's oldest
// rule. It is inert here not because the alarm outranks stopping, but because
// the machine is already stopped and HELD stopped by the alarm task - there is
// nothing left for CancelMotion to ask for, and MM_DECELLERATE would be
// overwritten within milliseconds anyway.
TEST(UiStateAlarm, HaltIsInert) {
  Rig r;
  r.tick(kAlarmMoving);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Halt, UiKeyEvent::Press, kAlarmMoving));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Halt, UiKeyEvent::Click, kAlarmMoving));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

TEST(UiStateAlarm, OkAsksForTheClearOnTheClickOnly) {
  Rig r;
  r.tick(kAlarm);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Press, kAlarm));
  EXPECT_EQ(UiIntent::ClearAlarm, r.key(UiKey::Ok, UiKeyEvent::Click, kAlarm));
  // Release must not fire a second reset pulse off one tap.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Release, kAlarm));
}

// The distinction the whole feature rests on: OK acknowledges, it does not
// dismiss. While the fault is still present the context still says alarm, so
// the modal is still up and OK still offers to try again.
TEST(UiStateAlarm, OkDoesNotDismissTheModalWhileTheFaultRemains) {
  Rig r;
  r.tick(kAlarm);
  ASSERT_EQ(UiIntent::ClearAlarm, r.key(UiKey::Ok, UiKeyEvent::Click, kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  r.tick(kAlarm);
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  EXPECT_EQ(UiIntent::ClearAlarm, r.key(UiKey::Ok, UiKeyEvent::Click, kAlarm));
}

TEST(UiStateAlarm, TheModalComesDownWhenTheFaultDoes) {
  Rig r;
  r.tick(kAlarm);
  ASSERT_EQ(UiFocus::Alarm, r.focus());
  EXPECT_TRUE(r.tick(kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_FALSE(r.tick(kNoStops));
}

// ...and a key event can be the thing that observes the clear, exactly as it
// can be the thing that observes the fault.
TEST(UiStateAlarm, AKeyAfterTheFaultClearsFindsTheRestScreen) {
  Rig r;
  r.tick(kAlarm);
  ASSERT_EQ(UiFocus::Alarm, r.focus());
  // MODE from the rest screen opens the MODE widget - i.e. this key was judged
  // against Jog, and not against a focus nothing below the gate has heard of.
  r.click(UiKey::Mode, kNoStops);
  EXPECT_EQ(UiFocus::Mode, r.focus());
}

// Motion is not restarted for the operator. Once the fault has cleared the
// machine is free again - but only because they ask: the first jog after an
// alarm is an ordinary jog, started by an ordinary arrow press.
TEST(UiStateAlarm, JoggingIsAvailableAgainOnceItClears) {
  Rig r;
  r.tick(kAlarm);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Left, UiKeyEvent::Press, kAlarm));
  r.tick(kNoStops);
  EXPECT_EQ(UiIntent::JogLeftStart,
            r.key(UiKey::Left, UiKeyEvent::Press, kNoStops));
}

// A fault that lands mid-jog. The arrow is still physically down when the modal
// goes up, so its Release arrives afterwards - and must be inert, because the
// jog it would terminate ended when the driver stopped stepping. If m_jogDir
// survived the trip, that Release would emit JogStop against an axis the alarm
// task is already holding at MM_DISABLED.
TEST(UiStateAlarm, AJogInFlightWhenItTripsIsForgotten) {
  Rig r;
  ASSERT_EQ(UiIntent::JogRightStart,
            r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  r.tick(kAlarmMoving);
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Right, UiKeyEvent::Release, kAlarmMoving));
  // And after the clear, the stale direction is not lurking either.
  r.tick(kNoStops);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
}

// Same for a powered run to a stop: the latch must not outlive the fault, or
// the first arrow click after the clear would cancel a run that is long over
// instead of starting a new one.
TEST(UiStateAlarm, APoweredRunLatchDoesNotSurviveIt) {
  Rig r;
  const UiContext idle = ctx(true, false);
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));
  r.tick(kAlarmMoving);
  r.tick(idle);
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

// The alarm outranks the motion lockout, not the other way round. Both are true
// for the first instants of a trip, and if the lockout were reached first it
// would swallow the OK - leaving no way to acknowledge the fault until the
// machine got round to reporting itself stopped.
TEST(UiStateAlarm, OkStillWorksWhileTheContextStillReportsMotion) {
  Rig r;
  r.tick(kAlarmMoving);
  EXPECT_EQ(UiFocus::Alarm, r.focus());
  EXPECT_EQ(UiIntent::ClearAlarm,
            r.key(UiKey::Ok, UiKeyEvent::Click, kAlarmMoving));
}

// The whole of an alarm can happen with no key event in it: the operator watches
// the modal appear, frees the crash and presses OK, all with an arrow still held
// down from the jog that was running when the driver faulted. tick() therefore
// has to retire the jog and the run latch itself - handleKey() cannot, because
// it is never called. Without that, the eventual Release emits a JogStop for a
// jog that ended when the driver stopped stepping.
TEST(UiStateAlarm, AJogIsRetiredEvenIfNoKeyEventEverSeesTheAlarm) {
  Rig r;
  ASSERT_EQ(UiIntent::JogRightStart,
            r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  r.tick(kAlarmMoving);   // trips
  r.tick(kAlarm);         // ...and settles
  r.tick(kNoStops);       // ...and clears, all without a key
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
}

// The modal does not expire. Four seconds is the widget timeout; a fault the
// operator has walked away from must still be on screen when they come back.
TEST(UiStateAlarm, HasNoIdleTimeout) {
  Rig r;
  r.tick(kAlarm);
  r.advance(kTimeout * 10);
  EXPECT_FALSE(r.tick(kAlarm));
  EXPECT_EQ(UiFocus::Alarm, r.focus());
}

// ---------------------------------------------------------------------------
// THE OTA SCREEN (UiFocus::Ota) - deferred piece #1 of the OTA follow-up.
//
// THE DESIGN QUESTION THIS SECTION ANSWERS: does OK-on-the-OTA-screen need a
// UiFocus forced the way UiFocus::Alarm is, or can ButtonPad route it without
// touching UiState at all (the OTA screen is selected by GlobalState::
// hasOTA()/GlobalOtaStatus today, NOT by a UiFocus - see the destination note
// on menuTileDestination() in uistate.h)?
//
// ANSWER: it needs the UiFocus, for a reason that has nothing to do with
// mirroring Alarm for its own sake: src/buttonpad.h says outright "Nothing in
// src/ is host-testable, so no new decision logic belongs here." OK's
// behaviour on this screen - which keys are inert, whether HALT is one of
// them, that the routing releases the instant hasOTA() goes false - is
// exactly the kind of decision this project insists on host-testing (the
// same argument that put ClearAlarm here instead of in ButtonPad). Routing it
// in ButtonPad would make it both untested and inconsistent with every other
// panel decision in this codebase.
//
// WHERE IT DIFFERS FROM ALARM, and why - both differences trace back to one
// fact: an alarm is ASYNCHRONOUS (a hardware fault, on its own schedule, that
// can land mid-widget or mid-jog) and OTA is not. OTA can only begin through
// MENU_SOFTWARE_UPDATE, which menuTileBlock() already refuses while
// motionActive, and menuTileDestination() sends every tile including this one
// to UiFocus::Jog before GlobalState::hasOTA() is ever set (src/
// buttonpad.cpp's applyIntent() calls setOTA() synchronously in the same
// keypress that closed the menu). So by the time ctx.ota can ever be true,
// UiState is already sitting at Jog with the menu closed, nothing in flight -
// unlike an alarm, which has to be able to preempt literally anything.
//   * HALT is inert here too, but for a DIFFERENT reason than Alarm's. Alarm
//     inerts it because the alarm task is actively re-publishing MM_DISABLED
//     every cycle, so a CancelMotion would be overwritten or worse. OTA has no
//     such re-publisher - timerCallback() (src/main.cpp) simply stops calling
//     leadscrew->update() at all while hasOTA() is true. But the effect is the
//     same: nothing can make ctx.motionActive true while ctx.ota is true
//     (motion cannot begin while ctx.ota gates every motion-starting key
//     inert, and it could not begin in the first place - menuTileBlock()
//     already refused the tile otherwise), so a CancelMotion has nothing to
//     ask for either. "The machine is already stopped and held stopped" (the
//     Alarm comment's phrase) transfers on that basis, not by mere analogy.
//   * ctx.ota is asserted for the WHOLE hasOTA() span - connecting, checking,
//     downloading, settled - not only a settled failure. The display shows
//     nothing else for any of those phases (Display::update() skips
//     drawOverlay() entirely whenever hasOTA() is true), so a key that changed
//     focus or a setting behind any of them would be exactly the invisible-
//     mutation hazard the Alarm modal exists to prevent, whether or not the
//     outcome has settled yet.
//   * OK always emits AckOta, unconditionally - UiState has no visibility
//     into OtaOutcome::requiresAck() (only the ota bool crosses into
//     UiContext), and it does not need any: OtaOutcome::acknowledge() is
//     documented as harmless before the outcome settles, so there is no
//     "wrong phase" for the intent to arrive in.
//   * No test here claims OTA can start mid-jog or mid-run the way an alarm
//     can trip mid-jog - it cannot, by the menuTileBlock()/menuTileDestination()
//     argument above. The equivalents below (AJogInFlightIsForgottenIfOtaSets
//     InAnyway, etc.) are DEFENSIVE: they pin that the same cleanup fires if a
//     future caller ever violates that invariant, not a claim that the panel
//     can reach that state today.
// ---------------------------------------------------------------------------

// The steady state of an update attempt: hasOTA() true, the machine reported
// at rest (which is the only way it can have started - see the block comment
// above).
const UiContext kOta = ctx(false, false, /*motionEnabled=*/false,
                           /*motionActive=*/false, /*threadMode=*/false,
                           /*alarm=*/false, /*ota=*/true);
// DEFENSIVE ONLY (see the block comment): ota true with the context still
// reporting motion. Not a state the panel can reach - menuTileBlock() refuses
// Software update while motionActive - but the cleanup this exercises (the
// jog direction and run latch not surviving) must hold regardless of how
// ctx.ota became true, on the same "erring towards stop is always safe"
// principle the rest of this file uses for the real motion lockout.
const UiContext kOtaMoving = ctx(false, false, /*motionEnabled=*/true,
                                 /*motionActive=*/true, /*threadMode=*/false,
                                 /*alarm=*/false, /*ota=*/true);

TEST(UiStateOta, TakesFocusOnTheTick) {
  Rig r;
  EXPECT_TRUE(r.tick(kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
  // Only on the transition, exactly like Alarm - not a redraw trigger for the
  // whole span of the update.
  EXPECT_FALSE(r.tick(kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
}

TEST(UiStateOta, ClosesAnOpenWidget) {
  // Should not be reachable in practice (Software update can only be reached
  // via the menu, and every tile closes the menu to Jog on activation) but the
  // gate must not assume that - a widget could in principle still be expiring
  // out from under it.
  Rig r;
  r.click(UiKey::Mode);
  ASSERT_EQ(UiFocus::Mode, r.focus());
  EXPECT_TRUE(r.tick(kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
}

TEST(UiStateOta, ClosesTheCarousel) {
  Rig r;
  r.click(UiKey::Menu);
  ASSERT_TRUE(r.ui().menuOpen());
  EXPECT_TRUE(r.tick(kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
  EXPECT_FALSE(r.ui().menuOpen());
}

TEST(UiStateOta, AKeyEventTakesFocusToo) {
  Rig r;
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Mode, UiKeyEvent::Click, kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
}

// The whole point: nothing the operator does behind the OTA screen may take
// effect invisibly, for the entire span hasOTA() is up - not only once a
// failure has settled and wants an acknowledgement.
TEST(UiStateOta, EveryKeyButOkIsInert) {
  Rig r;
  r.tick(kOta);
  const UiKey keys[] = {UiKey::Mode,  UiKey::Rate,   UiKey::Stops,
                        UiKey::Left,  UiKey::Right,  UiKey::Halt,
                        UiKey::Menu,  UiKey::Enable, UiKey::EncoderCw,
                        UiKey::EncoderCcw};
  const UiKeyEvent evs[] = {UiKeyEvent::Press, UiKeyEvent::Click,
                            UiKeyEvent::Hold, UiKeyEvent::Release};
  for (UiKey k : keys) {
    for (UiKeyEvent e : evs) {
      EXPECT_EQ(UiIntent::None, r.key(k, e, kOta))
          << k << " " << e << " acted during an OTA attempt";
      EXPECT_EQ(UiFocus::Ota, r.focus());
    }
  }
}

// HALT gets its own test for the same reason it does under Alarm: this file's
// oldest rule is "HALT is checked first, unconditionally", and this is a
// second, independent place that rule is deliberately NOT honoured to the
// letter. See the block comment above for why the "nothing left to stop"
// reasoning transfers from Alarm even though the mechanism differs.
TEST(UiStateOta, HaltIsInert) {
  Rig r;
  r.tick(kOtaMoving);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Halt, UiKeyEvent::Press, kOtaMoving));
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Halt, UiKeyEvent::Click, kOtaMoving));
  EXPECT_EQ(UiFocus::Ota, r.focus());
}

TEST(UiStateOta, OkAsksForTheAcknowledgementOnTheClickOnly) {
  Rig r;
  r.tick(kOta);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Press, kOta));
  EXPECT_EQ(UiIntent::AckOta, r.key(UiKey::Ok, UiKeyEvent::Click, kOta));
  // Release must not fire a second acknowledgement off one tap.
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Ok, UiKeyEvent::Release, kOta));
}

// OK acknowledges; it does not dismiss. The screen comes down only when
// GlobalState::hasOTA() itself goes false (the OTA task's own exitAction()),
// so pressing OK again before that still asks again - harmless, since
// OtaOutcome::acknowledge() is idempotent (m_acked latches true).
TEST(UiStateOta, OkDoesNotDismissTheScreenWhileOtaRemains) {
  Rig r;
  r.tick(kOta);
  ASSERT_EQ(UiIntent::AckOta, r.key(UiKey::Ok, UiKeyEvent::Click, kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
  r.tick(kOta);
  EXPECT_EQ(UiFocus::Ota, r.focus());
  EXPECT_EQ(UiIntent::AckOta, r.key(UiKey::Ok, UiKeyEvent::Click, kOta));
}

TEST(UiStateOta, TheScreenComesDownWhenOtaEnds) {
  Rig r;
  r.tick(kOta);
  ASSERT_EQ(UiFocus::Ota, r.focus());
  EXPECT_TRUE(r.tick(kNoStops));
  EXPECT_EQ(UiFocus::Jog, r.focus());
  EXPECT_FALSE(r.tick(kNoStops));
}

TEST(UiStateOta, AKeyAfterOtaEndsFindsTheRestScreen) {
  Rig r;
  r.tick(kOta);
  ASSERT_EQ(UiFocus::Ota, r.focus());
  r.click(UiKey::Mode, kNoStops);
  EXPECT_EQ(UiFocus::Mode, r.focus());
}

// DEFENSIVE (see the block comment): not reachable through the panel today,
// since Software update cannot be activated while a jog is in flight, but the
// cleanup must hold regardless.
TEST(UiStateOta, AJogInFlightIsForgottenIfOtaStartsAnyway) {
  Rig r;
  ASSERT_EQ(UiIntent::JogRightStart,
            r.key(UiKey::Right, UiKeyEvent::Press, kNoStops));
  r.tick(kOtaMoving);
  EXPECT_EQ(UiFocus::Ota, r.focus());
  EXPECT_EQ(UiIntent::None,
            r.key(UiKey::Right, UiKeyEvent::Release, kOtaMoving));
  r.tick(kNoStops);
  EXPECT_EQ(UiIntent::None, r.key(UiKey::Right, UiKeyEvent::Release, kNoStops));
}

// DEFENSIVE, same basis as the jog case above.
TEST(UiStateOta, APoweredRunLatchDoesNotSurviveItEitherIfOtaStartsAnyway) {
  Rig r;
  const UiContext idle = ctx(true, false);
  const UiContext running = ctx(true, false, /*motionEnabled=*/false,
                                /*motionActive=*/true);
  ASSERT_EQ(UiIntent::RunToLeftStop, r.startRun(UiKey::Left, idle, running));
  r.tick(kOtaMoving);
  EXPECT_EQ(UiFocus::Ota, r.focus());
  r.tick(idle);
  ASSERT_EQ(UiFocus::Jog, r.focus());
  EXPECT_EQ(UiIntent::RunToLeftStop, r.click(UiKey::Left, idle));
}

// OK must not be swallowed by the motion lockout in the same window an alarm
// has to survive it - defensive, on the same basis as the two tests above.
TEST(UiStateOta, OkStillWorksWhileTheContextStillReportsMotion) {
  Rig r;
  r.tick(kOtaMoving);
  EXPECT_EQ(UiFocus::Ota, r.focus());
  EXPECT_EQ(UiIntent::AckOta, r.key(UiKey::Ok, UiKeyEvent::Click, kOtaMoving));
}

// The screen does not expire on the 4 s widget timeout. An update the
// operator has walked away from - to fetch a tool, per the OtaOutcome header
// comment - must still be on screen when they come back, for as long as
// hasOTA() says so.
TEST(UiStateOta, HasNoIdleTimeout) {
  Rig r;
  r.tick(kOta);
  r.advance(kTimeout * 10);
  EXPECT_FALSE(r.tick(kOta));
  EXPECT_EQ(UiFocus::Ota, r.focus());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
