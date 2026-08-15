// Per-setting coverage for every LatheConfig lathe setting.
//
// For EACH configurable setting this suite:
//   * sets a non-default value and asserts every DEPENDENT derived value matches
//     the exact formula (expressed via the same float arithmetic so
//     EXPECT_FLOAT_EQ is exact), and
//   * asserts the INDEPENDENT derived values are unchanged versus the default
//     config (isolation), and
//   * where meaningful, drives the leadscrew and confirms carriage motion
//     reflects the change (ratio settings) or the direction pin flips
//     (invertDirection).
//
// Settings covered (defaults from latheconfig.h):
//   spindleEncoderPpr(1200), stepperPpr(400), invertDirection(true),
//   gearboxRatioNumerator(2), gearboxRatioDenominator(1),
//   leadscrewPitchMm(2.54), jogSpeed(40), leadscrewAcceleration(150),
//   leadscrewMaxSpeed(40).
//
// Dependency map (from latheconfig.cpp + main.cpp Leadscrew wiring):
//   leadscrewStepsPerMm = stepperPpr * (num/den) / leadscrewPitchMm
//   jogSpeedPps         = jogSpeed        * leadscrewStepsPerMm
//   leadscrewMaxSpeedPps= leadscrewMaxSpeed * leadscrewStepsPerMm
//   accellerationPulseSec = leadscrewAcceleration * leadscrewStepsPerMm
//   leadscrewInitialPulseDelay = US_PER_SECOND / (LEADSCREW_JERK * stepsPerMm)
//   gearboxRatio        = num/den
//   dirRight/dirLeft    = invertDirection ? 1/0 : 0/1
//   motion ratio        = pitch * (stepperPpr*gearboxRatio)
//                                 / (leadscrewPitchMm * spindleEncoderPpr)
// spindleEncoderPpr feeds ONLY the motion ratio, none of the other derived
// values.

#include <gmock/gmock.h>

#include <Arduino.h>

#include <cmath>
#include <vector>

#include "config.h"
#include "globalstate.h"
#include "latheconfig.h"
#include "leadscrew.h"
#include "leadscrewio_mock.h"
#include "spindle.h"

namespace {

// ---------------------------------------------------------------------------
// Derived-value snapshot helpers
// ---------------------------------------------------------------------------

// Every derived value, captured for one config. Used both for "does the
// dependent value match the formula" and "did the independent values stay put".
struct Derived {
  float stepsPerMm;
  float jogPps;
  float maxPps;
  float accelPps;
  float initialDelay;
  float gearboxRatio;
  int dirRight;
  int dirLeft;
  int encoderPpr;
  int stepperPpr;
  int jogSpeed;
  int accel;
  int maxSpeed;
  float pitch;
};

Derived snapshot(LatheConfig cfg) {
  cfg.check = CHECKVALUE;
  LatheConfigDerived d(&cfg);  // pointer to local cfg; valid within this scope
  Derived out;
  out.stepsPerMm = d.leadscrewStepsPerMm();
  out.jogPps = d.jogSpeedPps();
  out.maxPps = d.leadscrewMaxSpeedPps();
  out.accelPps = d.accellerationPulseSec();
  out.initialDelay = d.leadscrewInitialPulseDelay();
  out.gearboxRatio = d.gearboxRatio();
  out.dirRight = d.dirRight();
  out.dirLeft = d.dirLeft();
  out.encoderPpr = d.spindleEncoderPpr();
  out.stepperPpr = d.stepperPpr();
  out.jogSpeed = d.jogSpeed();
  out.accel = d.leadscrewAcceleration();
  out.maxSpeed = d.leadscrewMaxSpeed();
  out.pitch = d.leadscrewPitchMm();
  return out;
}

const Derived kDefault = snapshot(LatheConfig());

// Exact float re-computation of the derived formulas, matching latheconfig.cpp.
float stepsPerMmOf(int stepperPpr, int num, int den, float pitch) {
  return (float)stepperPpr * ((float)num / (float)den) / pitch;
}
float initialDelayOf(float stepsPerMm) {
  return (float)US_PER_SECOND / ((float)LEADSCREW_JERK * stepsPerMm);
}

}  // namespace

// ===========================================================================
// PART A: per-setting derived values + isolation
// ===========================================================================

// --- Defaults sanity: snapshot matches the documented default maths ---------
TEST(Settings_Defaults, DerivedValuesMatchFormula) {
  const float s = stepsPerMmOf(400, 2, 1, 2.54f);
  EXPECT_FLOAT_EQ(kDefault.stepsPerMm, s);
  EXPECT_FLOAT_EQ(kDefault.jogPps, 40.0f * s);
  EXPECT_FLOAT_EQ(kDefault.maxPps, 40.0f * s);
  EXPECT_FLOAT_EQ(kDefault.accelPps, 150.0f * s);
  EXPECT_FLOAT_EQ(kDefault.initialDelay, initialDelayOf(s));
  EXPECT_FLOAT_EQ(kDefault.gearboxRatio, 2.0f);
  EXPECT_EQ(kDefault.dirRight, 1);
  EXPECT_EQ(kDefault.dirLeft, 0);
}

// --- spindleEncoderPpr: affects motion ratio ONLY --------------------------
// None of the LatheConfigDerived values depend on encoderPpr, so every derived
// value must be byte-for-byte identical to the default snapshot.
TEST(Settings_SpindleEncoderPpr, DoesNotChangeAnyDerivedValue) {
  for (int ppr : {600, 2400, 3600}) {
    LatheConfig cfg;
    cfg.spindleEncoderPpr = ppr;
    Derived d = snapshot(cfg);

    EXPECT_EQ(d.encoderPpr, ppr);  // raw pass-through changed
    // Everything derived is unchanged (isolation).
    EXPECT_FLOAT_EQ(d.stepsPerMm, kDefault.stepsPerMm);
    EXPECT_FLOAT_EQ(d.jogPps, kDefault.jogPps);
    EXPECT_FLOAT_EQ(d.maxPps, kDefault.maxPps);
    EXPECT_FLOAT_EQ(d.accelPps, kDefault.accelPps);
    EXPECT_FLOAT_EQ(d.initialDelay, kDefault.initialDelay);
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- stepperPpr: linear on stepsPerMm chain, inverse on initialDelay --------
TEST(Settings_StepperPpr, ScalesStepsChainAndRatioNotGearboxOrDir) {
  for (int spp : {800, 200, 2000}) {
    LatheConfig cfg;
    cfg.stepperPpr = spp;
    Derived d = snapshot(cfg);

    const float s = stepsPerMmOf(spp, 2, 1, 2.54f);
    EXPECT_EQ(d.stepperPpr, spp);
    EXPECT_FLOAT_EQ(d.stepsPerMm, s);
    EXPECT_FLOAT_EQ(d.jogPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.maxPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.accelPps, 150.0f * s);
    EXPECT_FLOAT_EQ(d.initialDelay, initialDelayOf(s));

    // Isolation: gearbox ratio and direction pins untouched.
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- gearboxRatioNumerator: linear on gearboxRatio -> stepsPerMm chain -------
TEST(Settings_GearboxNumerator, ScalesRatioAndStepsChainNotDir) {
  for (int num : {3, 4, 1}) {
    LatheConfig cfg;
    cfg.gearboxRatioNumerator = num;  // denominator stays 1
    Derived d = snapshot(cfg);

    const float ratio = (float)num / 1.0f;
    const float s = stepsPerMmOf(400, num, 1, 2.54f);
    EXPECT_FLOAT_EQ(d.gearboxRatio, ratio);
    EXPECT_FLOAT_EQ(d.stepsPerMm, s);
    EXPECT_FLOAT_EQ(d.jogPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.maxPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.accelPps, 150.0f * s);
    EXPECT_FLOAT_EQ(d.initialDelay, initialDelayOf(s));

    // Isolation: direction pins untouched.
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- gearboxRatioDenominator: inverse on gearboxRatio -> stepsPerMm chain ----
TEST(Settings_GearboxDenominator, InverselyScalesRatioAndStepsChainNotDir) {
  // Use 3/2 (ratio 1.5) and 2/4 (ratio 0.5) to exercise a real fraction.
  struct Case { int num, den; };
  for (Case c : {Case{3, 2}, Case{2, 4}}) {
    LatheConfig cfg;
    cfg.gearboxRatioNumerator = c.num;
    cfg.gearboxRatioDenominator = c.den;
    Derived d = snapshot(cfg);

    const float ratio = (float)c.num / (float)c.den;
    const float s = stepsPerMmOf(400, c.num, c.den, 2.54f);
    EXPECT_FLOAT_EQ(d.gearboxRatio, ratio);
    EXPECT_FLOAT_EQ(d.stepsPerMm, s);
    EXPECT_FLOAT_EQ(d.jogPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.maxPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.accelPps, 150.0f * s);
    EXPECT_FLOAT_EQ(d.initialDelay, initialDelayOf(s));

    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- leadscrewPitchMm: inverse on stepsPerMm chain --------------------------
TEST(Settings_LeadscrewPitchMm, InverselyScalesStepsChainNotGearboxOrDir) {
  for (float pitch : {5.08f, 1.27f, 3.0f}) {
    LatheConfig cfg;
    cfg.leadscrewPitchMm = pitch;
    Derived d = snapshot(cfg);

    const float s = stepsPerMmOf(400, 2, 1, pitch);
    EXPECT_FLOAT_EQ(d.pitch, pitch);
    EXPECT_FLOAT_EQ(d.stepsPerMm, s);
    EXPECT_FLOAT_EQ(d.jogPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.maxPps, 40.0f * s);
    EXPECT_FLOAT_EQ(d.accelPps, 150.0f * s);
    EXPECT_FLOAT_EQ(d.initialDelay, initialDelayOf(s));

    // Isolation: gearbox ratio and direction pins untouched.
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- jogSpeed: linear on jogSpeedPps ONLY -----------------------------------
TEST(Settings_JogSpeed, ScalesJogPpsOnly) {
  for (int jog : {80, 10, 100}) {
    LatheConfig cfg;
    cfg.jogSpeed = jog;
    Derived d = snapshot(cfg);

    EXPECT_EQ(d.jogSpeed, jog);
    EXPECT_FLOAT_EQ(d.jogPps, (float)jog * kDefault.stepsPerMm);

    // Isolation: nothing else changes.
    EXPECT_FLOAT_EQ(d.stepsPerMm, kDefault.stepsPerMm);
    EXPECT_FLOAT_EQ(d.maxPps, kDefault.maxPps);
    EXPECT_FLOAT_EQ(d.accelPps, kDefault.accelPps);
    EXPECT_FLOAT_EQ(d.initialDelay, kDefault.initialDelay);
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- leadscrewAcceleration: linear on accellerationPulseSec ONLY ------------
TEST(Settings_LeadscrewAcceleration, ScalesAccelPpsOnly) {
  for (int accel : {300, 75, 500}) {
    LatheConfig cfg;
    cfg.leadscrewAcceleration = accel;
    Derived d = snapshot(cfg);

    EXPECT_EQ(d.accel, accel);
    EXPECT_FLOAT_EQ(d.accelPps, (float)accel * kDefault.stepsPerMm);

    // Isolation: nothing else changes.
    EXPECT_FLOAT_EQ(d.stepsPerMm, kDefault.stepsPerMm);
    EXPECT_FLOAT_EQ(d.jogPps, kDefault.jogPps);
    EXPECT_FLOAT_EQ(d.maxPps, kDefault.maxPps);
    EXPECT_FLOAT_EQ(d.initialDelay, kDefault.initialDelay);
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- leadscrewMaxSpeed: linear on leadscrewMaxSpeedPps ONLY -----------------
TEST(Settings_LeadscrewMaxSpeed, ScalesMaxPpsOnly) {
  for (int mx : {80, 20, 120}) {
    LatheConfig cfg;
    cfg.leadscrewMaxSpeed = mx;
    Derived d = snapshot(cfg);

    EXPECT_EQ(d.maxSpeed, mx);
    EXPECT_FLOAT_EQ(d.maxPps, (float)mx * kDefault.stepsPerMm);

    // Isolation: nothing else changes.
    EXPECT_FLOAT_EQ(d.stepsPerMm, kDefault.stepsPerMm);
    EXPECT_FLOAT_EQ(d.jogPps, kDefault.jogPps);
    EXPECT_FLOAT_EQ(d.accelPps, kDefault.accelPps);
    EXPECT_FLOAT_EQ(d.initialDelay, kDefault.initialDelay);
    EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
    EXPECT_EQ(d.dirRight, kDefault.dirRight);
    EXPECT_EQ(d.dirLeft, kDefault.dirLeft);
  }
}

// --- invertDirection: swaps dir pins ONLY -----------------------------------
TEST(Settings_InvertDirection, SwapsDirPinsOnly) {
  LatheConfig cfg;
  cfg.invertDirection = false;  // default is true
  Derived d = snapshot(cfg);

  EXPECT_EQ(d.dirRight, 0);
  EXPECT_EQ(d.dirLeft, 1);

  // Isolation: every numeric derived value is unchanged.
  EXPECT_FLOAT_EQ(d.stepsPerMm, kDefault.stepsPerMm);
  EXPECT_FLOAT_EQ(d.jogPps, kDefault.jogPps);
  EXPECT_FLOAT_EQ(d.maxPps, kDefault.maxPps);
  EXPECT_FLOAT_EQ(d.accelPps, kDefault.accelPps);
  EXPECT_FLOAT_EQ(d.initialDelay, kDefault.initialDelay);
  EXPECT_FLOAT_EQ(d.gearboxRatio, kDefault.gearboxRatio);
}

// ===========================================================================
// PART B: motion-reflection spot checks
// ===========================================================================
//
// A leadscrew rig (TestSpindle-style, virtual clock, LeadscrewIOMock) is built
// from an arbitrary LatheConfig, mirroring the fixture in
// test/test_thread_sync/test_thread_sync.cpp and the main.cpp wiring.

namespace {

constexpr uint64_t kDt = 100;  // 100 us of virtual time per update()

// Drive the spindle by `pulses` (signed) net encoder counts at |pps| rate,
// one kDt step at a time, calling ls->update() each step. Fractional pulses are
// carried between steps so the average rate is exactly |pps|.
void driveSpindle(Spindle *spindle, Leadscrew *ls, int pulses, int pps) {
  const int dir = pulses >= 0 ? 1 : -1;
  const int target = pulses >= 0 ? pulses : -pulses;
  const double perStep = (double)pps * (double)kDt / 1e6;
  double carry = 0.0;
  int delivered = 0;
  while (delivered < target) {
    advanceMockMicros(kDt);
    carry += perStep;
    int whole = (int)carry;
    if (whole > 0) {
      if (delivered + whole > target) whole = target - delivered;
      carry -= (double)((int)carry);
      spindle->incrementCurrentPosition(dir * whole);
      delivered += whole;
    }
    ls->update();
  }
}

void settle(Leadscrew *ls, int maxSteps = 60000) {
  int stableFor = 0;
  int last = ls->getCurrentPosition();
  for (int i = 0; i < maxSteps; ++i) {
    advanceMockMicros(kDt);
    ls->update();
    int now = ls->getCurrentPosition();
    if (now == last) {
      if (++stableFor > 1000) return;
    } else {
      stableFor = 0;
      last = now;
    }
  }
}

// Fixture that owns the GlobalState singleton reset and the per-config rig.
class SettingsMotionTest : public ::testing::Test {
 protected:
  GlobalState *gs = nullptr;
  void SetUp() override { gs = GlobalState::getInstance(); }

  // Build spindle + leadscrew from `cfg` exactly as main.cpp wires them, sync a
  // thread at the LEFT stop (carriage 0), free the axis, drive `revs` whole
  // spindle revolutions forward at `pps`, settle, and return carriage travel in
  // leadscrew steps. The config/derived live for the duration of the call.
  int travelForRevs(LatheConfig cfg, float pitch, int revs, int pps) {
    cfg.check = CHECKVALUE;
    LatheConfigDerived derived(&cfg);

    resetMockClock();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);

    Spindle spindle(0, 1, &derived);
    LeadscrewIOMock io;
    Leadscrew ls(&derived, &spindle, &io,
                 derived.accellerationPulseSec(),
                 derived.leadscrewInitialPulseDelay(),
                 derived.stepperPpr() * derived.gearboxRatio(),
                 derived.leadscrewPitchMm(), derived.spindleEncoderPpr());

    ls.setTargetPitchMM(pitch);
    ls.setCurrentPosition(0);
    ls.setStopPosition(LeadscrewStopPosition::LEFT, 0);  // establishes sync
    EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
    ls.unsetStopPosition(LeadscrewStopPosition::LEFT);  // free travel

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(&spindle, &ls, revs * derived.spindleEncoderPpr(), pps);
    settle(&ls);
    return ls.getCurrentPosition();
  }
};

// Expected leadscrew travel (steps) for `revs` whole spindle revolutions:
//   revs * pitch * leadscrewStepsPerMm.
float expectedTravel(LatheConfig cfg, float pitch, int revs) {
  Derived d = snapshot(cfg);
  return (float)revs * pitch * d.stepsPerMm;
}

// Absolute-travel tolerance. After the spindle stops, the axis decelerates and
// parks a few discrete steps short of the exact target (there is no endstop to
// pin it, unlike the thread_sync tests). That deceleration tail is ~1% of
// travel, so allow max(4 steps, 1%). The per-setting RATIO checks below pin the
// relationship far more tightly than this.
float travelTol(float expected) {
  float pct = (expected < 0 ? -expected : expected) * 0.01f;
  return pct > 4.0f ? pct : 4.0f;
}

}  // namespace

// The core invariant across all ratio settings:
//   leadscrew travel per whole spindle revolution
//     == thread pitch in leadscrew steps == pitch * leadscrewStepsPerMm.
// We assert this holds for the default config first, then as each ratio input
// (stepperPpr, gearbox num/den, leadscrewPitchMm, spindleEncoderPpr) changes.

TEST_F(SettingsMotionTest, TravelPerRevEqualsPitchInSteps_Default) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig cfg;  // defaults
  int travel = travelForRevs(cfg, pitch, revs, 1200);
  const float exp = expectedTravel(cfg, pitch, revs);
  EXPECT_NEAR((float)travel, exp, travelTol(exp));
}

TEST_F(SettingsMotionTest, StepperPprScalesTravel) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig base;
  LatheConfig dbl;
  dbl.stepperPpr = 800;  // double

  int tBase = travelForRevs(base, pitch, revs, 1200);
  int tDbl = travelForRevs(dbl, pitch, revs, 1200);

  // Each matches pitch*stepsPerMm for its own config...
  const float eBase = expectedTravel(base, pitch, revs);
  const float eDbl = expectedTravel(dbl, pitch, revs);
  EXPECT_NEAR((float)tBase, eBase, travelTol(eBase));
  EXPECT_NEAR((float)tDbl, eDbl, travelTol(eDbl));
  // ...and doubling stepperPpr doubles the travel.
  EXPECT_NEAR((double)tDbl / (double)tBase, 2.0, 0.02);
}

TEST_F(SettingsMotionTest, GearboxNumeratorScalesTravel) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig base;             // 2/1
  LatheConfig hi;
  hi.gearboxRatioNumerator = 4;  // 4/1 -> ratio doubles

  int tBase = travelForRevs(base, pitch, revs, 1200);
  int tHi = travelForRevs(hi, pitch, revs, 1200);

  const float eBase = expectedTravel(base, pitch, revs);
  const float eHi = expectedTravel(hi, pitch, revs);
  EXPECT_NEAR((float)tBase, eBase, travelTol(eBase));
  EXPECT_NEAR((float)tHi, eHi, travelTol(eHi));
  EXPECT_NEAR((double)tHi / (double)tBase, 2.0, 0.02);
}

TEST_F(SettingsMotionTest, GearboxDenominatorInverselyScalesTravel) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig base;              // 2/1
  LatheConfig lo;
  lo.gearboxRatioDenominator = 2;  // 2/2 -> ratio halves

  int tBase = travelForRevs(base, pitch, revs, 1200);
  int tLo = travelForRevs(lo, pitch, revs, 1200);

  const float eBase = expectedTravel(base, pitch, revs);
  const float eLo = expectedTravel(lo, pitch, revs);
  EXPECT_NEAR((float)tBase, eBase, travelTol(eBase));
  EXPECT_NEAR((float)tLo, eLo, travelTol(eLo));
  EXPECT_NEAR((double)tLo / (double)tBase, 0.5, 0.02);
}

TEST_F(SettingsMotionTest, LeadscrewPitchMmInverselyScalesTravel) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig base;               // 2.54
  LatheConfig coarse;
  coarse.leadscrewPitchMm = 5.08f;  // double the leadscrew pitch -> half travel

  int tBase = travelForRevs(base, pitch, revs, 1200);
  int tCoarse = travelForRevs(coarse, pitch, revs, 1200);

  const float eBase = expectedTravel(base, pitch, revs);
  const float eCoarse = expectedTravel(coarse, pitch, revs);
  EXPECT_NEAR((float)tBase, eBase, travelTol(eBase));
  EXPECT_NEAR((float)tCoarse, eCoarse, travelTol(eCoarse));
  EXPECT_NEAR((double)tCoarse / (double)tBase, 0.5, 0.02);
}

// spindleEncoderPpr changes travel PER PULSE but NOT travel per whole
// revolution (which stays pinned to pitch * stepsPerMm, independent of PPR).
TEST_F(SettingsMotionTest, EncoderPprLeavesTravelPerRevUnchanged) {
  const float pitch = 0.5f;
  const int revs = 5;
  LatheConfig base;             // 1200
  LatheConfig hi;
  hi.spindleEncoderPpr = 2400;  // double the encoder resolution

  int tBase = travelForRevs(base, pitch, revs, 1200);
  // Drive at the same rpm: 2x PPR -> 2x pps for the same revs of spindle.
  int tHi = travelForRevs(hi, pitch, revs, 2400);

  const float expected = expectedTravel(base, pitch, revs);  // PPR-independent
  EXPECT_NEAR((float)tBase, expected, travelTol(expected));
  EXPECT_NEAR((float)tHi, expected, travelTol(expected));
  EXPECT_NEAR((float)tHi, (float)tBase, 4.0f)
      << "travel per whole revolution should not depend on encoder PPR";
}

// invertDirection: for the SAME travel direction (carriage moving RIGHT), the
// direction pin written must be dirRight() -> 1 when inverted, 0 when not.
TEST_F(SettingsMotionTest, InvertDirectionSetsDirPinForRightTravel) {
  const float pitch = 0.5f;
  for (bool invert : {true, false}) {
    LatheConfig cfg;
    cfg.invertDirection = invert;
    cfg.check = CHECKVALUE;
    LatheConfigDerived derived(&cfg);

    resetMockClock();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);

    Spindle spindle(0, 1, &derived);
    LeadscrewIOMock io;
    Leadscrew ls(&derived, &spindle, &io,
                 derived.accellerationPulseSec(),
                 derived.leadscrewInitialPulseDelay(),
                 derived.stepperPpr() * derived.gearboxRatio(),
                 derived.leadscrewPitchMm(), derived.spindleEncoderPpr());

    ls.setTargetPitchMM(pitch);
    ls.setCurrentPosition(0);
    ls.setStopPosition(LeadscrewStopPosition::LEFT, 0);
    ls.unsetStopPosition(LeadscrewStopPosition::LEFT);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    // Positive pitch + forward spindle => carriage travels RIGHT.
    driveSpindle(&spindle, &ls, 1200, 1200);

    EXPECT_EQ(ls.getCurrentDirection(), LeadscrewDirection::RIGHT);
    EXPECT_EQ((int)io.m_dirPinState, derived.dirRight())
        << "dir pin for RIGHT travel should equal dirRight() (invert="
        << invert << ")";
    // And the two settings genuinely differ.
    EXPECT_EQ(derived.dirRight(), invert ? 1 : 0);
  }
}

// ===========================================================================
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
