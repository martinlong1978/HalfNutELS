// =============================================================================
// REGRESSION-DOCUMENTATION SUITE  (INTENTIONALLY FAILS ON master)
// =============================================================================
//
// These tests encode the PRE-WEB-SETTINGS golden values (firmware at commit
// cbd1b22, before commit b0b6137 "Made all lathe properties configurable via
// web") as the behaviour-preserving expectation. They therefore FAIL on the
// current master, which is exactly the point: each failure documents a concrete
// numeric regression.
//
// ROOT CAUSE
// ----------
// Old config.h:
//     ELS_LEADSCREW_STEPS_PER_MM
//         = (ELS_LEADSCREW_STEPPER_PPR * ELS_GEARBOX_RATIO) / ELS_LEADSCREW_PITCH_MM
//         = (400 * 2) / 2.54
//         = 314.9606
//
// New LatheConfigDerived::leadscrewStepsPerMm() (latheconfig.cpp):
//     return stepperPpr() * (gearboxRatioNumerator()/gearboxRatioDenominator())
//         = 400 * 2
//         = 800
//
// The `/ leadscrewPitchMm` divisor was DROPPED. Every value derived from
// steps-per-mm (speed limits, acceleration, initial pulse delay) is now
// inflated by a factor of leadscrewPitchMm (2.54x at defaults): the machine
// accelerates ~2.54x harder and caps speed ~2.54x higher than the pre-web
// firmware. Thread-PITCH tracking is unaffected because Leadscrew's m_ratio is
// computed from motorPulsePerRevolution/leadscrewPitch/encoderPPR directly, not
// from steps-per-mm.
//
// DECISION: per the project owner, this regression is REPORT-ONLY. The FIX is
// DEFERRED. These tests are the executable record of the defect; they are kept
// in a SEPARATE suite so the passing suites (test_latheconfig, test_leadscrew,
// test_smoke) stay green:
//     pio test -e native -f test_latheconfig    # green
//     pio test -e native -f test_regression     # red (by design)
// =============================================================================
#include <gmock/gmock.h>

#include "config.h"
#include "latheconfig.h"

namespace {

// Golden values from the pre-web-settings firmware (commit cbd1b22) at the
// default hardware configuration.
constexpr float kOldStepsPerMm = 314.9606f;         // (400*2)/2.54
constexpr float kOldInitialPulseDelayUs = 6350.0f;  // 1e6/(0.5*314.9606)
constexpr float kOldJogSpeedPps = 12598.4f;         // 40 * 314.9606
constexpr float kOldMaxSpeedPps = 12598.4f;         // 40 * 314.9606
constexpr float kOldAccelPulseSec = 47244.0f;       // 150 * 314.9606

LatheConfig makeDefaultConfig() {
  LatheConfig cfg;
  cfg.check = CHECKVALUE;
  return cfg;
}

}  // namespace

// Each EXPECT below is expected to FAIL on master (observed value in comment).
TEST(PreWebSettingsRegression, LeadscrewStepsPerMm_DroppedPitchDivisor) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);
  // Expected (old): 314.96 ; Actual (master): 800
  EXPECT_NEAR(d.leadscrewStepsPerMm(), kOldStepsPerMm, 0.5f);
}

TEST(PreWebSettingsRegression, LeadscrewInitialPulseDelay) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);
  // Expected (old): 6350.0 us ; Actual (master): 2500.0 us
  EXPECT_NEAR(d.leadscrewInitialPulseDelay(), kOldInitialPulseDelayUs, 1.0f);
}

TEST(PreWebSettingsRegression, JogSpeedPps) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);
  // Expected (old): 12598.4 ; Actual (master): 32000
  EXPECT_NEAR(d.jogSpeedPps(), kOldJogSpeedPps, 1.0f);
}

TEST(PreWebSettingsRegression, LeadscrewMaxSpeedPps) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);
  // Expected (old): 12598.4 ; Actual (master): 32000
  EXPECT_NEAR(d.leadscrewMaxSpeedPps(), kOldMaxSpeedPps, 1.0f);
}

TEST(PreWebSettingsRegression, AccellerationPulseSec) {
  LatheConfig cfg = makeDefaultConfig();
  LatheConfigDerived d(&cfg);
  // Expected (old): 47244 ; Actual (master): 120000
  EXPECT_NEAR(d.accellerationPulseSec(), kOldAccelPulseSec, 1.0f);
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
