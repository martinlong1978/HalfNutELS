// Pin the behaviour of LatheConfigDerived (pure math, no Arduino stub
// required). These tests MUST PASS: they document the derived-value maths so
// that refactors can be verified against them.
//
// The default-config values below match the pre-web-settings firmware: the
// steps-per-mm regression (a dropped /leadscrewPitchMm divisor) has been fixed,
// so leadscrewStepsPerMm and everything derived from it are back to the
// original scaling.
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

// --- Derived values at defaults -------------------------------------------
TEST(LatheConfigDerivedDefaults, DerivedValues) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);

  // stepperPpr * (num/den) / leadscrewPitchMm = 400 * 2 / 2.54 ~= 314.96
  const float stepsPerMm = 400.0f * (2.0f / 1.0f) / 2.54f;
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), stepsPerMm);

  // jogSpeed(40) * stepsPerMm ~= 12598.4
  EXPECT_FLOAT_EQ(d.jogSpeedPps(), 40.0f * stepsPerMm);

  // leadscrewMaxSpeed(40) * stepsPerMm ~= 12598.4
  EXPECT_FLOAT_EQ(d.leadscrewMaxSpeedPps(), 40.0f * stepsPerMm);

  // leadscrewAcceleration(150) * stepsPerMm ~= 47244.1
  EXPECT_FLOAT_EQ(d.accellerationPulseSec(), 150.0f * stepsPerMm);

  // US_PER_SECOND / (LEADSCREW_JERK(0.5) * stepsPerMm) = 6350
  EXPECT_FLOAT_EQ(d.leadscrewInitialPulseDelay(), 1000000.0f / (0.5f * stepsPerMm));

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
  // 400 * 1.5 / 2.54 ~= 236.22
  const float stepsPerMm = 400.0f * (3.0f / 2.0f) / 2.54f;
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), stepsPerMm);
  EXPECT_FLOAT_EQ(d.jogSpeedPps(), 40.0f * stepsPerMm);
}

// --- Stepper PPR scaling --------------------------------------------------
TEST(LatheConfigDerivedNonDefault, StepperPprScalesStepsPerMm) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.stepperPpr = 800;  // e.g. 1/2 microstepping change
  LatheConfigDerived d(&cfg);

  // 800 * 2 / 2.54 ~= 629.92
  const float stepsPerMm = 800.0f * (2.0f / 1.0f) / 2.54f;
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), stepsPerMm);
  // initial pulse delay = 1e6 / (0.5 * stepsPerMm) = 3175
  EXPECT_FLOAT_EQ(d.leadscrewInitialPulseDelay(), 1000000.0f / (0.5f * stepsPerMm));
}

// --- leadscrewPitchMm scales steps-per-mm inversely ------------------------
TEST(LatheConfigDerivedNonDefault, PitchAffectsStepsPerMm) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.leadscrewPitchMm = 5.08f;  // double the default pitch
  LatheConfigDerived d(&cfg);

  // steps-per-mm is inversely proportional to leadscrew pitch, so doubling the
  // pitch halves steps-per-mm: 400 * 2 / 5.08 ~= 157.48
  EXPECT_FLOAT_EQ(d.leadscrewStepsPerMm(), 800.0f / 5.08f);
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
