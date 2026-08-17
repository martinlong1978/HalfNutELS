// Tests for FS-B item 1 (docs/ux-redesign.md §8 "Units", §9 item 1): the
// carriage position and stop positions must be readable in millimetres, and
// the derived config must be reachable off Leadscrew so the display can
// format units.
//
// These target THREE new methods that do not yet have a real implementation
// (declared in lib/leadscrew/leadscrew.h with deliberately-wrong stub bodies
// so this suite fails on ASSERTION, not link error):
//   float Leadscrew::getPositionMM()
//   float Leadscrew::getStopPositionMM(LeadscrewStopPosition)
//   LatheConfigDerived* Leadscrew::getConfig()
//
// Contract for getStopPositionMM on an UNSET stop: return NAN (checked with
// std::isnan). getStopPositionState() remains the way to ask "is it set" —
// callers must not blindly convert the INT32_MIN/INT32_MAX pulse sentinels to
// mm, since dividing those by steps-per-mm is meaningless.
#include <gmock/gmock.h>

#include <cmath>

#include <Arduino.h>

#include "config.h"
#include "globalstate.h"
#include "latheconfig.h"
#include "leadscrew.h"
#include "leadscrewio_mock.h"
#include "spindle.h"

namespace {

const float kEpsilonMM = 0.001f;

// Ctor args mirror how src/main.cpp wires the leadscrew at defaults (same
// pattern as test/test_leadscrew/test_leadscrew.cpp).
class PositionMMTest : public ::testing::Test {
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

    gs = GlobalState::getInstance();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);

    ls = new Leadscrew(
        derived, spindle, &io,
        derived->accellerationPulseSec(),
        derived->leadscrewInitialPulseDelay(),
        derived->stepperPpr() * derived->gearboxRatio(),
        derived->leadscrewPitchMm(),
        derived->spindleEncoderPpr());
  }

  void TearDown() override {
    delete ls;
    delete spindle;
    delete derived;
  }
};

// --------------------------------------------------------------------------
// 1. getPositionMM() — converts getCurrentPosition() (pulses) to mm via
//    config->leadscrewStepsPerMm(). Expected value is derived from the
//    config, not hardcoded, so this survives a config change.
// --------------------------------------------------------------------------
TEST_F(PositionMMTest, ZeroPositionIsZeroMM) {
  ls->setCurrentPosition(0);
  EXPECT_NEAR(ls->getPositionMM(), 0.0f, kEpsilonMM);
}

TEST_F(PositionMMTest, PositivePositionConvertsToMM) {
  ls->setCurrentPosition(1000);
  float expected = 1000.0f / derived->leadscrewStepsPerMm();
  EXPECT_NEAR(ls->getPositionMM(), expected, kEpsilonMM);
}

TEST_F(PositionMMTest, NegativePositionConvertsToMM) {
  ls->setCurrentPosition(-500);
  float expected = -500.0f / derived->leadscrewStepsPerMm();
  EXPECT_NEAR(ls->getPositionMM(), expected, kEpsilonMM);
}

TEST_F(PositionMMTest, LargePositionConvertsToMM) {
  ls->setCurrentPosition(123456);
  float expected = 123456.0f / derived->leadscrewStepsPerMm();
  EXPECT_NEAR(ls->getPositionMM(), expected, kEpsilonMM);
}

// --------------------------------------------------------------------------
// 2. getStopPositionMM() when SET.
// --------------------------------------------------------------------------
TEST_F(PositionMMTest, SetLeftStopPositionMM) {
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -2540);
  float expected = -2540.0f / derived->leadscrewStepsPerMm();
  EXPECT_NEAR(ls->getStopPositionMM(LeadscrewStopPosition::LEFT), expected,
              kEpsilonMM);
}

TEST_F(PositionMMTest, SetRightStopPositionMM) {
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 2540);
  float expected = 2540.0f / derived->leadscrewStepsPerMm();
  EXPECT_NEAR(ls->getStopPositionMM(LeadscrewStopPosition::RIGHT), expected,
              kEpsilonMM);
}

// --------------------------------------------------------------------------
// 3. Unset stops must be distinguishable — the sentinels (INT32_MIN /
//    INT32_MAX) must NOT be blindly converted to mm. Chosen contract:
//    getStopPositionMM() returns NAN when the stop is UNSET.
//    getStopPositionState() remains the authority on "is it set".
// --------------------------------------------------------------------------
TEST_F(PositionMMTest, UnsetLeftStopPositionMMIsNan) {
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  EXPECT_TRUE(std::isnan(ls->getStopPositionMM(LeadscrewStopPosition::LEFT)));
}

TEST_F(PositionMMTest, UnsetRightStopPositionMMIsNan) {
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::UNSET);
  EXPECT_TRUE(
      std::isnan(ls->getStopPositionMM(LeadscrewStopPosition::RIGHT)));
}

// --------------------------------------------------------------------------
// 4. Round-trip consistency: setting a stop at the CURRENT position must
//    read back the same mm value as getPositionMM() at that moment.
// --------------------------------------------------------------------------
TEST_F(PositionMMTest, RoundTripLeftStopMatchesCurrentPositionMM) {
  ls->setCurrentPosition(777);
  ls->setStopPosition(LeadscrewStopPosition::LEFT);  // stops at current pos
  EXPECT_NEAR(ls->getStopPositionMM(LeadscrewStopPosition::LEFT),
              ls->getPositionMM(), kEpsilonMM);
}

TEST_F(PositionMMTest, RoundTripRightStopMatchesCurrentPositionMM) {
  ls->setCurrentPosition(-321);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT);  // stops at current pos
  EXPECT_NEAR(ls->getStopPositionMM(LeadscrewStopPosition::RIGHT),
              ls->getPositionMM(), kEpsilonMM);
}

// --------------------------------------------------------------------------
// 5. Config accessor — the display needs the derived config to format units.
// --------------------------------------------------------------------------
TEST_F(PositionMMTest, GetConfigReturnsNonNull) {
  ASSERT_NE(ls->getConfig(), nullptr);
}

TEST_F(PositionMMTest, GetConfigMatchesConstructedDerivedConfig) {
  ASSERT_NE(ls->getConfig(), nullptr);
  // Pointer identity, not just value equality: getConfig() must hand back the
  // exact LatheConfigDerived the Leadscrew was constructed with, not merely
  // one that happens to compute the same numbers (which a stray second
  // instance with identical defaults would also satisfy).
  EXPECT_EQ(ls->getConfig(), derived);
  EXPECT_FLOAT_EQ(ls->getConfig()->leadscrewStepsPerMm(),
                   derived->leadscrewStepsPerMm());
}

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
