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

// Every byte value must resolve to one of the two enumerators, and ONLY the
// DRO_DATUM_RIGHT byte may produce Right. The three spot values above would
// still pass if the helper mapped, say, 0x80 to Right; this closes that.
TEST(LatheConfigDroDatumMapping, EveryByteResolvesAndOnlyOneMapsToRight) {
  for (int b = 0; b <= 0xFF; ++b) {
    DroDatumPreference expected = (b == DRO_DATUM_RIGHT) ? DroDatumPreference::Right
                                                         : DroDatumPreference::Left;
    EXPECT_EQ(toDroDatumPreference((uint8_t)b), expected) << "stored byte " << b;
  }
}

// --- Device-writable preferences ------------------------------------------
// `theme` and `droDatum` are the only two LatheConfig fields the device itself
// may write; everything else is commissioning geometry, set over Wi-Fi only.
// The flash side of that (saveLathePreferences(), src/WebSettings.cpp) is NOT
// host-testable - src/ is not built for the native env and ESP.flashRead/
// flashEraseSector/flashWrite have no native stub. What IS testable, and is
// tested here, is the part that decides which bytes change.

TEST(LatheConfigTheme, NormaliseAcceptsTheTwoKnownThemes) {
  EXPECT_EQ(normaliseTheme(THEME_DARK), (uint8_t)THEME_DARK);
  EXPECT_EQ(normaliseTheme(THEME_LIGHT), (uint8_t)THEME_LIGHT);
}

// Same hazard as the droDatum byte: flash can hold anything, and an unknown
// value must resolve to the safe default rather than being trusted.
TEST(LatheConfigTheme, NormaliseFallsBackToDarkForUnknownBytes) {
  EXPECT_EQ(normaliseTheme(2), (uint8_t)THEME_DARK);
  EXPECT_EQ(normaliseTheme(0x7F), (uint8_t)THEME_DARK);
  EXPECT_EQ(normaliseTheme(0xFF), (uint8_t)THEME_DARK);
}

// ...and ONLY the THEME_LIGHT byte may produce light, across the whole range.
TEST(LatheConfigTheme, EveryByteNormalisesAndOnlyOneMapsToLight) {
  for (int b = 0; b <= 0xFF; ++b) {
    const uint8_t expected = (b == THEME_LIGHT) ? THEME_LIGHT : THEME_DARK;
    EXPECT_EQ(normaliseTheme((uint8_t)b), expected) << "stored byte " << b;
  }
}

TEST(LatheConfigTheme, ToggleFlipsBetweenTheTwoThemes) {
  EXPECT_EQ(toggleTheme(THEME_DARK), (uint8_t)THEME_LIGHT);
  EXPECT_EQ(toggleTheme(THEME_LIGHT), (uint8_t)THEME_DARK);
  // Toggling twice is the identity for both valid values - a menu tile pressed
  // an even number of times must land where it started.
  EXPECT_EQ(toggleTheme(toggleTheme(THEME_DARK)), (uint8_t)THEME_DARK);
  EXPECT_EQ(toggleTheme(toggleTheme(THEME_LIGHT)), (uint8_t)THEME_LIGHT);
}

// A garbage stored byte normalises to dark, so toggling it yields light - a
// defined outcome, not whatever the raw byte happened to compare against.
TEST(LatheConfigTheme, ToggleOfAnUnknownByteYieldsLight) {
  EXPECT_EQ(toggleTheme(0xFF), (uint8_t)THEME_LIGHT);
  EXPECT_EQ(toggleTheme(42), (uint8_t)THEME_LIGHT);
}

TEST(LatheConfigDroDatumMapping, ToggleFlipsBetweenTheTwoDatums) {
  EXPECT_EQ(toggleDroDatum(DroDatumPreference::Left), DroDatumPreference::Right);
  EXPECT_EQ(toggleDroDatum(DroDatumPreference::Right), DroDatumPreference::Left);
  EXPECT_EQ(toggleDroDatum(toggleDroDatum(DroDatumPreference::Left)),
            DroDatumPreference::Left);
}

// THE test for this whole mechanism. applyLathePreferences() is what a
// device-side save applies to the LatheConfig it just read back out of flash,
// so it must touch the two preference bytes and NOTHING else: every geometry
// field must survive byte-for-byte, or a theme toggle would rewrite the user's
// commissioned machine. Deliberately uses non-default geometry throughout - a
// default-valued config would pass even if the function replaced the struct
// wholesale, which is exactly the bug being guarded against.
TEST(LatheConfigPreferences, ApplyChangesOnlyThemeAndDatum) {
  LatheConfig cfg;
  cfg.check = CHECKVALUE;
  cfg.spindleEncoderPpr = 2048;
  cfg.stepperPpr = 800;
  cfg.invertDirection = false;
  cfg.gearboxRatioNumerator = 5;
  cfg.gearboxRatioDenominator = 3;
  cfg.leadscrewPitchMm = 3.175f;
  cfg.jogSpeed = 17;
  cfg.leadscrewAcceleration = 275;
  cfg.leadscrewMaxSpeed = 22;
  cfg.theme = THEME_DARK;
  cfg.droDatum = DRO_DATUM_LEFT;

  applyLathePreferences(cfg, THEME_LIGHT, DroDatumPreference::Right);

  // The two bytes it is allowed to write.
  EXPECT_EQ(cfg.theme, (uint8_t)THEME_LIGHT);
  EXPECT_EQ(cfg.droDatum, (uint8_t)DRO_DATUM_RIGHT);

  // ...and every commissioning value, unchanged.
  EXPECT_EQ(cfg.spindleEncoderPpr, 2048);
  EXPECT_EQ(cfg.stepperPpr, 800);
  EXPECT_FALSE(cfg.invertDirection);
  EXPECT_EQ(cfg.gearboxRatioNumerator, 5);
  EXPECT_EQ(cfg.gearboxRatioDenominator, 3);
  EXPECT_FLOAT_EQ(cfg.leadscrewPitchMm, 3.175f);
  EXPECT_EQ(cfg.jogSpeed, 17);
  EXPECT_EQ(cfg.leadscrewAcceleration, 275);
  EXPECT_EQ(cfg.leadscrewMaxSpeed, 22);
}

// The validity sentinel is NOT the caller's to change here. Stamping CHECKVALUE
// on a blob this firmware did not recognise would promote unknown bytes to
// "valid geometry"; the decision to trust a blob is made before this point.
TEST(LatheConfigPreferences, ApplyLeavesTheCheckValueAlone) {
  LatheConfig valid;
  valid.check = CHECKVALUE;
  applyLathePreferences(valid, THEME_LIGHT, DroDatumPreference::Right);
  EXPECT_EQ((uint32_t)valid.check, (uint32_t)CHECKVALUE);

  LatheConfig unrecognised;
  unrecognised.check = 0x12345678;
  applyLathePreferences(unrecognised, THEME_LIGHT, DroDatumPreference::Right);
  EXPECT_EQ((uint32_t)unrecognised.check, 0x12345678u);
}

// The bytes written are normalised, so a garbage theme byte cannot be laundered
// into flash by passing it straight through.
TEST(LatheConfigPreferences, ApplyNormalisesTheThemeByteItWrites) {
  LatheConfig cfg;
  cfg.check = CHECKVALUE;
  applyLathePreferences(cfg, 0xFF, DroDatumPreference::Left);

  EXPECT_EQ(cfg.theme, (uint8_t)THEME_DARK);
  EXPECT_EQ(cfg.droDatum, (uint8_t)DRO_DATUM_LEFT);
}

// Round trip: what applyLathePreferences() writes is what readLathePreferences()
// would read back out of the same struct (the flash hop in between is the part
// that has to be verified on device).
TEST(LatheConfigPreferences, WrittenBytesReadBackAsTheSamePreferences) {
  LatheConfig cfg;
  cfg.check = CHECKVALUE;

  applyLathePreferences(cfg, THEME_LIGHT, DroDatumPreference::Right);
  EXPECT_EQ(normaliseTheme(cfg.theme), (uint8_t)THEME_LIGHT);
  EXPECT_EQ(toDroDatumPreference(cfg.droDatum), DroDatumPreference::Right);

  applyLathePreferences(cfg, THEME_DARK, DroDatumPreference::Left);
  EXPECT_EQ(normaliseTheme(cfg.theme), (uint8_t)THEME_DARK);
  EXPECT_EQ(toDroDatumPreference(cfg.droDatum), DroDatumPreference::Left);
}

// --- Flash layout ---------------------------------------------------------
// LatheConfig is blitted raw into flash (src/WebSettings.cpp), so its size and
// member offsets are the on-disk format. latheconfig.h carries static_asserts
// that fail the BUILD if any offset moves; these mirror them at runtime so the
// intent is visible in the test suite and a failure reads as a sentence rather
// than a compiler error. Keep the two in step.
TEST(LatheConfigFlashLayout, SizeAndMemberOffsetsAreFrozen) {
  EXPECT_EQ(sizeof(LatheConfig), 44u);
  EXPECT_EQ(offsetof(LatheConfig, check), 0u);
  EXPECT_EQ(offsetof(LatheConfig, spindleEncoderPpr), 4u);
  EXPECT_EQ(offsetof(LatheConfig, stepperPpr), 8u);
  EXPECT_EQ(offsetof(LatheConfig, invertDirection), 12u);
  EXPECT_EQ(offsetof(LatheConfig, gearboxRatioNumerator), 16u);
  EXPECT_EQ(offsetof(LatheConfig, gearboxRatioDenominator), 20u);
  EXPECT_EQ(offsetof(LatheConfig, leadscrewPitchMm), 24u);
  EXPECT_EQ(offsetof(LatheConfig, jogSpeed), 28u);
  EXPECT_EQ(offsetof(LatheConfig, leadscrewAcceleration), 32u);
  EXPECT_EQ(offsetof(LatheConfig, leadscrewMaxSpeed), 36u);
}

// The two ux-redesign fields were appended so that no existing member moved.
// This asserts the specific property that made that safe: they sit past the
// end of the pre-redesign struct (which was 40 bytes), so a stored blob
// written by older firmware overlaps only the older members.
TEST(LatheConfigFlashLayout, NewFieldsSitPastThePreRedesignStructSize) {
  EXPECT_GE(offsetof(LatheConfig, theme), 40u);
  EXPECT_EQ(offsetof(LatheConfig, theme), 40u);
  EXPECT_EQ(offsetof(LatheConfig, droDatum), 41u);
  // ...and the struct still ends 4-byte aligned, which is what keeps the
  // flashWrite length legal and LatheConfig's flash offset aligned.
  EXPECT_EQ(sizeof(LatheConfig) % 4, 0u);
}

// Pins the KNOWN BLIND SPOT in the sizeof assert, so nobody mistakes it for a
// guard against fields being added. droDatum ends at byte 42 and the struct is
// 44 bytes, so two more uint8_t fields fit in the trailing padding with
// sizeof(LatheConfig) unchanged - the size assert would stay silent. That is
// why the per-member offset asserts are the load-bearing ones, and why
// src/buttonpad.cpp no longer relies on a size assert to prove that a
// field-by-field copy of this struct is complete: it no longer makes one.
TEST(LatheConfigFlashLayout, SizeAssertCannotSeeFieldsAppendedIntoTrailingPad) {
  const size_t lastUsedByte = offsetof(LatheConfig, droDatum) + 1;
  EXPECT_EQ(lastUsedByte, 42u);
  EXPECT_EQ(sizeof(LatheConfig) - lastUsedByte, 2u)
      << "trailing padding: this many uint8_t fields could be appended without "
         "sizeof(LatheConfig) changing";
}

// A settings blob is only trusted when check == CHECKVALUE. Erased flash reads
// back as all-0xFF and a blank/zeroed region as all-0x00, so CHECKVALUE must
// equal neither - otherwise a wiped sector (including the window mid-save,
// between flashEraseSector and flashWrite) would be accepted as valid settings
// instead of dropping the device into AP setup.
TEST(LatheConfigCheckValue, DoesNotCollideWithErasedOrBlankFlash) {
  EXPECT_NE((uint32_t)CHECKVALUE, 0xFFFFFFFFu);
  EXPECT_NE((uint32_t)CHECKVALUE, 0x00000000u);
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
