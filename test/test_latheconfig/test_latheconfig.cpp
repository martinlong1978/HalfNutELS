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

// --- ux-redesign: theme / DRO datum persistence (docs/ux-redesign.md
// section 6 "Saving settings from the device", section 8 "Theme") ----------
//
// LatheConfig is memcpy'd raw to/from flash, so the two new fields must
// survive that: plain uint8_t, sensible defaults, and CHECKVALUE bumped so
// devices holding a shorter pre-redesign blob re-adopt defaults instead of
// reading flash garbage into them.

// A default-constructed LatheConfig (no flash read at all - just the C++
// in-class defaults) must already carry sane values for the new fields, the
// same way every other field above does.
TEST(LatheConfigTheme, DefaultConstructedConfigHasDarkThemeAndLeftDatum) {
  LatheConfig cfg;

  EXPECT_EQ(cfg.theme, THEME_DARK);
  EXPECT_EQ(cfg.droDatum, DRO_DATUM_LEFT);
}

// CHECKVALUE must differ from the pre-redesign constant: this is the guard
// that stops a future field addition from shipping garbage settings to every
// device already in the field (see the comment on CHECKVALUE in
// latheconfig.h). Written as a literal, not a re-derivation, so it fails
// loudly if someone reverts the bump rather than silently comparing itself.
TEST(LatheConfigCheckValue, DiffersFromPreRedesignConstant) {
  EXPECT_NE((uint32_t)CHECKVALUE, 0xDFEB0940u);
}

// --- LatheConfigDerived pass-through accessors, matching the existing style
TEST(LatheConfigDerivedDefaults, ThemeAndDatumAccessors) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);

  EXPECT_EQ(d.theme(), THEME_DARK);
  EXPECT_EQ(d.droDatum(), DroDatumPreference::Left);
}

TEST(LatheConfigDerivedNonDefault, ThemeAndDatumAccessorsReflectStoredValues) {
  LatheConfig cfg = makeDefaultConfig();
  cfg.theme = THEME_LIGHT;
  cfg.droDatum = DRO_DATUM_RIGHT;
  LatheConfigDerived d(&cfg);

  EXPECT_EQ(d.theme(), THEME_LIGHT);
  EXPECT_EQ(d.droDatum(), DroDatumPreference::Right);
}

// --- droDatum <-> DroDatumPreference mapping, both directions --------------
TEST(LatheConfigDroDatumMapping, LeftByteMapsToLeftPreference) {
  EXPECT_EQ(toDroDatumPreference(DRO_DATUM_LEFT), DroDatumPreference::Left);
}

TEST(LatheConfigDroDatumMapping, RightByteMapsToRightPreference) {
  EXPECT_EQ(toDroDatumPreference(DRO_DATUM_RIGHT), DroDatumPreference::Right);
}

TEST(LatheConfigDroDatumMapping, LeftPreferenceMapsToLeftByte) {
  EXPECT_EQ(fromDroDatumPreference(DroDatumPreference::Left), (uint8_t)DRO_DATUM_LEFT);
}

TEST(LatheConfigDroDatumMapping, RightPreferenceMapsToRightByte) {
  EXPECT_EQ(fromDroDatumPreference(DroDatumPreference::Right), (uint8_t)DRO_DATUM_RIGHT);
}

TEST(LatheConfigDroDatumMapping, RoundTripsBothWays) {
  EXPECT_EQ(toDroDatumPreference(fromDroDatumPreference(DroDatumPreference::Left)),
            DroDatumPreference::Left);
  EXPECT_EQ(toDroDatumPreference(fromDroDatumPreference(DroDatumPreference::Right)),
            DroDatumPreference::Right);
}

// An out-of-range stored byte is exactly what flash garbage looks like (a
// short-read pre-redesign blob, bit rot, an uninitialised sector). It must
// fall back to the safe default (Left) rather than being cast blindly into
// the enum - an arbitrary byte value must never masquerade as a
// DroDatumPreference. Cover several garbage values, not just one.
TEST(LatheConfigDroDatumMapping, OutOfRangeByteFallsBackToLeft) {
  EXPECT_EQ(toDroDatumPreference(2), DroDatumPreference::Left);
  EXPECT_EQ(toDroDatumPreference(0xFF), DroDatumPreference::Left);
  EXPECT_EQ(toDroDatumPreference(0x7F), DroDatumPreference::Left);
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
