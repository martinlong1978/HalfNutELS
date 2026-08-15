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

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
