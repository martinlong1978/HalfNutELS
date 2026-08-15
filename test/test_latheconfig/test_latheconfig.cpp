// Pin CURRENT behaviour of LatheConfigDerived (pure math, no Arduino stub
// required). These tests MUST PASS on master: they document what the code does
// today so that low-risk refactors can be verified against them.
//
// NOTE: The default-config values asserted here are the *current* (post
// web-settings) values, which are known to differ from the pre-web-settings
// firmware. The separate test_regression suite documents that discrepancy.
#include <gmock/gmock.h>

#include "config.h"
#include "latheconfig.h"

namespace {

// A fresh default LatheConfig, matching the struct defaults in latheconfig.h:
//   spindleEncoderPpr = 1200, stepperPpr = 400, invertDirection = true,
//   gearboxRatio = 2/1, leadscrewPitchMm = 2.54, jogSpeed = 40,
//   leadscrewAcceleration = 150, leadscrewMaxSpeed = 40.
LatheConfig makeDefaultConfig() {
  LatheConfig cfg;
  cfg.check = CHECKVALUE;
  return cfg;
}

}  // namespace

// --- Raw pass-through accessors -------------------------------------------
TEST(LatheConfigDerivedDefaults, RawAccessors) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);

  EXPECT_EQ(d.spindleEncoderPpr(), 1200);
  EXPECT_EQ(d.stepperPpr(), 400);
  EXPECT_TRUE(d.invertDirection());
  EXPECT_EQ(d.gearboxRatioNumerator(), 2);
  EXPECT_EQ(d.gearboxRatioDenominator(), 1);
  EXPECT_FLOAT_EQ(d.leadscrewPitchMm(), 2.54f);
  EXPECT_EQ(d.jogSpeed(), 40);
  EXPECT_EQ(d.leadscrewAcceleration(), 150);
  EXPECT_EQ(d.leadscrewMaxSpeed(), 40);
}

// --- Derived values at defaults (current behaviour) ------------------------
TEST(LatheConfigDerivedDefaults, DerivedValues) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);

  // stepperPpr * (num/den) = 400 * 2 = 800  (NOTE: no /leadscrewPitchMm)
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), 800.0f);

  // jogSpeed(40) * stepsPerMm(800) = 32000
  EXPECT_FLOAT_EQ(d.jogSpeedPps(), 32000.0f);

  // leadscrewMaxSpeed(40) * stepsPerMm(800) = 32000
  EXPECT_FLOAT_EQ(d.leadscrewMaxSpeedPps(), 32000.0f);

  // leadscrewAcceleration(150) * stepsPerMm(800) = 120000
  EXPECT_FLOAT_EQ(d.accellerationPulseSec(), 120000.0f);

  // US_PER_SECOND / (LEADSCREW_JERK(0.5) * stepsPerMm(800)) = 1e6/400 = 2500
  EXPECT_FLOAT_EQ(d.leadscrewInitialPulseDelay(), 2500.0f);

  EXPECT_FLOAT_EQ(d.gearboxRatio(), 2.0f);
  EXPECT_EQ(d.dirRight(), 1);
  EXPECT_EQ(d.dirLeft(), 0);
}

// --- Direction inversion --------------------------------------------------
TEST(LatheConfigDerivedNonDefault, InvertDirectionFalseFlipsPins) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.invertDirection = false;
  LatheConfigDerived d(&cfg);

  EXPECT_FALSE(d.invertDirection());
  EXPECT_EQ(d.dirRight(), 0);
  EXPECT_EQ(d.dirLeft(), 1);
}

// --- Gearbox ratio scaling ------------------------------------------------
TEST(LatheConfigDerivedNonDefault, GearboxRatioScalesStepsPerMm) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.gearboxRatioNumerator = 3;
  cfg.gearboxRatioDenominator = 2;  // ratio 1.5
  LatheConfigDerived d(&cfg);

  EXPECT_FLOAT_EQ(d.gearboxRatio(), 1.5f);
  // 400 * 1.5 = 600
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), 600.0f);
  EXPECT_FLOAT_EQ(d.jogSpeedPps(), 40.0f * 600.0f);
}

// --- Stepper PPR scaling --------------------------------------------------
TEST(LatheConfigDerivedNonDefault, StepperPprScalesStepsPerMm) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.stepperPpr = 800;  // e.g. 1/2 microstepping change
  LatheConfigDerived d(&cfg);

  // 800 * 2 = 1600
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), 1600.0f);
  // initial pulse delay = 1e6 / (0.5 * 1600) = 1250
  EXPECT_FLOAT_EQ(d.leadscrewInitialPulseDelay(), 1250.0f);
}

// --- leadscrewPitchMm does NOT influence steps-per-mm (documents the bug) ---
TEST(LatheConfigDerivedNonDefault, PitchDoesNotAffectStepsPerMm) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.leadscrewPitchMm = 5.08f;  // double the pitch
  LatheConfigDerived d(&cfg);

  // Current behaviour: steps-per-mm is independent of pitch.
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), 800.0f);
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
