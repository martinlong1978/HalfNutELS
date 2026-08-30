// Characterization tests for Leadscrew on the host, using:
//   - the virtual-clock Arduino stub (micros()/millis()),
//   - the native Spindle double (TestSpindle.cpp),
//   - LeadscrewIOMock.
//
// These PIN observable CURRENT behaviour (they do not judge it). Values were
// obtained by observing the running code, then asserted exactly.
#include <gmock/gmock.h>

#include <Arduino.h>

#include "config.h"
#include "globalstate.h"
#include "latheconfig.h"
#include "leadscrew.h"
#include "leadscrewio_mock.h"
#include "spindle.h"

namespace {

// Ctor args mirror how src/main.cpp wires the leadscrew at defaults.
class LeadscrewTest : public ::testing::Test {
 protected:
  LatheConfig cfg;
  LatheConfigDerived *derived = nullptr;
  Spindle *spindle = nullptr;
  LeadscrewIOMock io;
  Leadscrew *ls = nullptr;
  GlobalState *gs = nullptr;

  void SetUp() override {
    resetMockClock();
    cfg = LatheConfig();
    cfg.check = CHECKVALUE;
    derived = new LatheConfigDerived(&cfg);
    spindle = new Spindle(0, 1, derived);

    // Reset the singleton to a known baseline for each test.
    gs = GlobalState::getInstance();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);

    ls = new Leadscrew(
        derived, spindle, &io,
        derived->accellerationPulseSec(),      // leadscrewAccel
        derived->leadscrewInitialPulseDelay(), // initialPulseDelay
        derived->stepperPpr() * derived->gearboxRatio(), // motorPulsePerRevolution = 800
        derived->leadscrewPitchMm(),           // 2.54
        derived->spindleEncoderPpr());         // 1200
  }

  void TearDown() override {
    delete ls;
    delete spindle;
    delete derived;
  }
};

// --------------------------------------------------------------------------
// Stop position semantics
// --------------------------------------------------------------------------
TEST_F(LeadscrewTest, StopPositionsUnsetByDefault) {
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::UNSET);
  // Unset defaults: LEFT -> INT32_MIN, RIGHT -> INT32_MAX.
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), INT32_MIN);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::RIGHT), INT32_MAX);
}

TEST_F(LeadscrewTest, SetAndGetStopPositionExplicit) {
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -500);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 500);

  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::SET);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::SET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), -500);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::RIGHT), 500);
}

TEST_F(LeadscrewTest, SetStopPositionUsesCurrentPosition) {
  ls->setCurrentPosition(1234);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::RIGHT), 1234);
}

TEST_F(LeadscrewTest, UnsetStopPositionRevertsToDefault) {
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -500);
  ls->unsetStopPosition(LeadscrewStopPosition::LEFT);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), INT32_MIN);
}

// --------------------------------------------------------------------------
// Position error
// --------------------------------------------------------------------------
TEST_F(LeadscrewTest, PositionErrorZeroInitially) {
  EXPECT_FLOAT_EQ(ls->getPositionError(), 0.0f);
}

// --------------------------------------------------------------------------
// Direction defaults
// --------------------------------------------------------------------------
TEST_F(LeadscrewTest, DirectionUnknownInitially) {
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);
}

// --------------------------------------------------------------------------
// update() behaviour
//
// The virtual clock is advanced by a large, fixed step before each update() so
// that the per-pulse timing gate (m_currentPulseDelay, always <= the initial
// pulse delay of 2500us) is always satisfied. Under ELS_USE_RMT (which board.h
// defines), Leadscrew::sendPulse() reports success unconditionally, so each
// qualifying update() emits exactly ONE pulse and moves the position by exactly
// one step. This makes the trajectory fully deterministic.
// --------------------------------------------------------------------------
namespace {
// Advance the clock past the pulse gate and run one update, n times.
void pump(Leadscrew *ls, int n, uint64_t step = 100000) {
  for (int i = 0; i < n; ++i) {
    advanceMockMicros(step);
    ls->update();
  }
}
}  // namespace

TEST_F(LeadscrewTest, DisabledModeDoesNotMove) {
  gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
  pump(ls, 10);
  EXPECT_EQ(ls->getCurrentPosition(), 0);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);
  EXPECT_EQ(io.m_stepPinState, 0);
}

TEST_F(LeadscrewTest, JogRightAdvancesOneStepPerUpdate) {
  gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
  pump(ls, 10);
  // Exactly one pulse per qualifying update, moving in +ve direction.
  EXPECT_EQ(ls->getCurrentPosition(), 10);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::RIGHT);
  EXPECT_EQ(io.m_dirPinState, derived->dirRight());  // dirRight() == 1 at defaults
}

TEST_F(LeadscrewTest, JogLeftRetreatsOneStepPerUpdate) {
  gs->setMotionMode(GlobalMotionMode::MM_JOG_LEFT);
  pump(ls, 10);
  EXPECT_EQ(ls->getCurrentPosition(), -10);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::LEFT);
  EXPECT_EQ(io.m_dirPinState, derived->dirLeft());  // dirLeft() == 0 at defaults
}

// Establish spindle sync without leaving an endstop armed (setting a stop at the
// current position is the only way to establish sync; we immediately unset it,
// which clears the sync-position state but leaves the thread-sync flag SYNC).
TEST_F(LeadscrewTest, EnabledModeFollowsSpindleWhenSynced) {
  ls->setStopPosition(LeadscrewStopPosition::LEFT);  // at current pos 0 -> SYNC
  EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
  ls->unsetStopPosition(LeadscrewStopPosition::LEFT);  // clear the armed endstop

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  // Advance the spindle so there is unconsumed motion to follow. At the default
  // feed pitch the leadscrew:spindle ratio makes the expected position advance
  // to ~200 pulses, well beyond the range we sample below.
  spindle->incrementCurrentPosition(3048);

  // Stay comfortably inside the accelerate-toward-target regime so each update
  // is exactly one step in the +ve direction (no deceleration/overshoot yet).
  pump(ls, 50);
  EXPECT_EQ(ls->getCurrentPosition(), 50);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::RIGHT);
  EXPECT_EQ(io.m_dirPinState, derived->dirRight());
  // expectedPosition (~200) minus currentPosition (50).
  EXPECT_FLOAT_EQ(ls->getPositionError(), 150.0f);
}

// Jogging into an armed endstop halts motion and disables the leadscrew.
TEST_F(LeadscrewTest, JogIntoRightEndstopStopsAndDisables) {
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 5);
  gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);

  pump(ls, 400);
  int settled = ls->getCurrentPosition();
  pump(ls, 100);

  // Motion has come to rest (deceleration completed) ...
  EXPECT_EQ(ls->getCurrentPosition(), settled);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);
  // ... the endstop was crossed, and the leadscrew disabled itself.
  EXPECT_GE(settled, 5);
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
}

// Reverse thread = negated ratio: the same spindle motion drives the leadscrew
// in the opposite direction (from the left stop toward the right). Mirrors
// EnabledModeFollowsSpindleWhenSynced with a negated pitch.
TEST_F(LeadscrewTest, NegativePitchReversesTravelDirection) {
  ls->setStopPosition(LeadscrewStopPosition::LEFT);  // at current pos 0 -> SYNC
  ls->unsetStopPosition(LeadscrewStopPosition::LEFT);  // clear the armed endstop

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  // Negate the target pitch, exactly as FM_THREAD_REVERSE does via
  // getCurrentFeedPitch(). Same magnitude as the default forward test (0.25).
  ls->setTargetPitchMM(-0.25f);
  spindle->incrementCurrentPosition(3048);

  pump(ls, 50);
  // Opposite sign to the forward case: travels LEFT, one step per update.
  EXPECT_EQ(ls->getCurrentPosition(), -50);
  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::LEFT);
  EXPECT_EQ(io.m_dirPinState, derived->dirLeft());
}

// --------------------------------------------------------------------------
// Issue #11 / round 2: the bit-flag design of the NEW GlobalMotionMode values
// MM_HOLD_JOG_LEFT/RIGHT (lib/global_state/globalstate.h). Pure enum
// arithmetic - no Leadscrew object needed - so this pins the mask reasoning
// on its own, independent of whether Leadscrew::update() has been taught to
// use the new modes yet (test_endstop_deadlock/test_endstop_deadlock.cpp
// pins the behavioural side: arrest + speed).
//
// Requirement (from the report): jogMode must come out true for the new
// modes (so Leadscrew::update() discards m_expectedPosition and bypasses the
// resync gate for them, exactly as it does for MM_JOG_*), and nothing keyed
// on MMF_INTERACTIVEJOG or MMF_JOG_DIRECTION may be disturbed - in
// particular MMF_INTERACTIVEJOG must NOT match the new modes, since that bit
// is what currently means "exempt from the endstop arrest" and the whole
// point of MM_HOLD_JOG_* is that it is NOT exempt.
// --------------------------------------------------------------------------
TEST(GlobalMotionModeMasks, HoldJogModesAreDistinctFromEveryExistingValue) {
  const GlobalMotionMode existing[] = {
      MM_UNSET,       MM_DISABLED,          MM_ENABLED,
      MM_JOG_LEFT,    MM_JOG_RIGHT,         MM_INTERACTIVE_JOG_LEFT,
      MM_INTERACTIVE_JOG_RIGHT, MM_DECELLERATE};
  for (GlobalMotionMode m : existing) {
    EXPECT_NE(m, MM_HOLD_JOG_LEFT);
    EXPECT_NE(m, MM_HOLD_JOG_RIGHT);
  }
  EXPECT_NE(MM_HOLD_JOG_LEFT, MM_HOLD_JOG_RIGHT);
}

TEST(GlobalMotionModeMasks, HoldJogSatisfiesTheSameJogTestLeadscrewUpdateUses) {
  // The exact expression from Leadscrew::update(): bool jogMode =
  // (m_motionMode & MMF_JOG) == MMF_JOG.
  EXPECT_EQ((MM_HOLD_JOG_LEFT & MMF_JOG), (int)MMF_JOG);
  EXPECT_EQ((MM_HOLD_JOG_RIGHT & MMF_JOG), (int)MMF_JOG);
  // Regression: the modes jogMode already had to get right are untouched.
  EXPECT_EQ((MM_JOG_LEFT & MMF_JOG), (int)MMF_JOG);
  EXPECT_EQ((MM_JOG_RIGHT & MMF_JOG), (int)MMF_JOG);
  EXPECT_EQ((MM_INTERACTIVE_JOG_LEFT & MMF_JOG), (int)MMF_JOG);
  EXPECT_EQ((MM_INTERACTIVE_JOG_RIGHT & MMF_JOG), (int)MMF_JOG);
  EXPECT_EQ((MM_DECELLERATE & MMF_JOG), (int)MMF_JOG);
  EXPECT_NE((MM_ENABLED & MMF_JOG), (int)MMF_JOG);
  EXPECT_NE((MM_DISABLED & MMF_JOG), (int)MMF_JOG);
}

TEST(GlobalMotionModeMasks, HoldJogDoesNotMatchTheInteractiveExemptionBit) {
  // MMF_INTERACTIVEJOG is the bit every "drives straight through a stop"
  // comment in leadscrew.cpp keys its reasoning on (see
  // InteractiveJogDrivesStraightThroughAStop in test_endstop_deadlock). The
  // new modes MUST arrest, so this bit must stay clear for them - setting it
  // would let a future refactor from the hardcoded MM_INTERACTIVE_JOG_LEFT/
  // RIGHT equality checks to `(mode & MMF_INTERACTIVEJOG)` silently exempt
  // the hold-jog too.
  EXPECT_NE((MM_HOLD_JOG_LEFT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  EXPECT_NE((MM_HOLD_JOG_RIGHT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  // Regression: still matches only the modes it always matched.
  EXPECT_EQ((MM_INTERACTIVE_JOG_LEFT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  EXPECT_EQ((MM_INTERACTIVE_JOG_RIGHT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  EXPECT_NE((MM_JOG_LEFT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  EXPECT_NE((MM_JOG_RIGHT & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
  EXPECT_NE((MM_DECELLERATE & MMF_INTERACTIVEJOG), (int)MMF_INTERACTIVEJOG);
}

TEST(GlobalMotionModeMasks, HoldJogDirectionMatchesTheExistingDirectionMask) {
  // MMF_JOG_DIRECTION (bit0|bit2) is "a jog-family mode going RIGHT". The new
  // RIGHT mode must match it and the new LEFT mode must not, the same as the
  // two existing jog pairs.
  EXPECT_EQ((MM_HOLD_JOG_RIGHT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
  EXPECT_NE((MM_HOLD_JOG_LEFT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
  // Regression: existing pairs unaffected.
  EXPECT_EQ((MM_JOG_RIGHT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
  EXPECT_NE((MM_JOG_LEFT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
  EXPECT_EQ((MM_INTERACTIVE_JOG_RIGHT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
  EXPECT_NE((MM_INTERACTIVE_JOG_LEFT & MMF_JOG_DIRECTION), (int)MMF_JOG_DIRECTION);
}

TEST(GlobalMotionModeMasks, HoldJogIsNotAnEnableStateMode) {
  // MMF_ENABLESTATE is the MM_ENABLED/MM_DISABLED pair; the hold-jog is
  // neither.
  EXPECT_NE((MM_HOLD_JOG_LEFT & MMF_ENABLESTATE), (int)MMF_ENABLESTATE);
  EXPECT_NE((MM_HOLD_JOG_RIGHT & MMF_ENABLESTATE), (int)MMF_ENABLESTATE);
  EXPECT_EQ((MM_ENABLED & MMF_ENABLESTATE), (int)MMF_ENABLESTATE);
  EXPECT_EQ((MM_DISABLED & MMF_ENABLESTATE), (int)MMF_ENABLESTATE);
}

TEST(GlobalMotionModeMasks, MmfHoldjogIdentifiesOnlyTheNewModes) {
  // The new mask this round adds. Nothing reads it in production code today
  // (Leadscrew::update() still needs the equality checks scoped in the
  // report), but it documents which bit distinguishes MM_HOLD_JOG_* and
  // pins that no existing value accidentally carries it.
  EXPECT_EQ((MM_HOLD_JOG_LEFT & MMF_HOLDJOG), (int)MMF_HOLDJOG);
  EXPECT_EQ((MM_HOLD_JOG_RIGHT & MMF_HOLDJOG), (int)MMF_HOLDJOG);
  const GlobalMotionMode existing[] = {
      MM_UNSET,       MM_DISABLED,          MM_ENABLED,
      MM_JOG_LEFT,    MM_JOG_RIGHT,         MM_INTERACTIVE_JOG_LEFT,
      MM_INTERACTIVE_JOG_RIGHT, MM_DECELLERATE};
  for (GlobalMotionMode m : existing) {
    EXPECT_NE((m & MMF_HOLDJOG), (int)MMF_HOLDJOG) << "mode=" << (int)m;
  }
}

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
