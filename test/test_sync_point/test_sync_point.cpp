// Tests for Leadscrew::setSyncPoint() - "pick up an existing thread".
// (docs/ux-redesign.md Sec. 6, MENU -> Sync tile.)
//
// THE FEATURE
// -----------
// The workpiece already has a thread on it - cut in a previous session, or the
// tool was pulled and reground. To recut or finish it the leadscrew must
// re-enter the SAME helix, or the tool ploughs a new groove across the existing
// one and the part is scrap. The gesture is: stop the spindle, hand-position the
// tool so it sits in the existing groove, then declare "this spindle angle and
// this carriage position are in sync". setSyncPoint() is that declaration.
//
// Today the spindle-sync anchor is only ever latched as a SIDE EFFECT of
// setStop() (and only when no anchor exists AND the stop lands exactly on the
// current carriage position). There is no way to say "I am in the groove here".
//
// WHAT THE ANCHOR ACTUALLY DOES  (important for reading these tests)
// ------------------------------------------------------------------
// The anchor value is used in exactly ONE place: the re-sync block in
// Leadscrew::update(). While `syncArmed()` and the thread-sync state is
// SS_UNSYNC, update() refuses to emit a single step until the spindle reaches
// the phase at which the carriage's CURRENT position lies on the anchored helix:
//
//   onHelixPhase(L) = positiveModulo((int)((L - syncCarriagePos)/ratio)
//                                    + syncSpindlePhase, encoderPPR)
//
// Everything else about the motion is a pure RELATIVE feed - the leadscrew
// simply accumulates ratio * (spindle delta). So the only way to observe WHICH
// helix is anchored is to break sync (disengage) and re-engage: the carriage
// then stalls until the phase comes round, and where it ends up afterwards is
// determined by the anchor. Tests that never disengage cannot tell a correct
// anchor from a missing one - hence disengageAndReEngage() below, which is the
// heart of most of this suite.
//
// CONVENTIONS
// -----------
// Modelled on test/test_thread_sync: the spindle is turned at a constant rate
// over the virtual clock (kDt of virtual time per update() call, integer encoder
// pulses with the fraction carried), so TestSpindle reports a realistic
// time-based velocity and the leadscrew's speed-dependent feed-forward is
// genuinely exercised. Speeds are swept 300/1200/3000 PPS (PPR 1200 ->
// 15/60/150 rpm).

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

// Simulation step: 100 us of virtual time per update() call.
constexpr uint64_t kDt = 100;
// Realistic spindle speeds in encoder pulses/sec (PPR 1200 -> 15/60/150 rpm).
const int kSpeeds[] = {300, 1200, 3000};

// 1.27 mm/rev (20 TPI) is chosen deliberately over test_thread_sync's 0.25:
// with the default config (stepperPpr 400 * gearbox 2 = 800 motor pulses/rev,
// leadscrew pitch 2.54 mm, encoder PPR 1200) it gives ratio = 1016/3048 = 1/3
// exactly and 400 leadscrew pulses per spindle revolution - so "one whole pitch
// of carriage travel" is an exact integer number of steps and the helix
// assertions below are exact rather than fighting rounding. kSanity* assert
// this, so a config change breaks loudly instead of quietly weakening the test.
constexpr float kPitchMm = 1.27f;
constexpr int kPulsesPerPitch = 400;
constexpr int kPPR = 1200;

// Carriage travel tolerance, in leadscrew pulses. Discrete acceleration can
// leave the axis a step either side of the ideal.
constexpr int kTravelTol = 2;
// How far the carriage may sit off the anchored helix, in leadscrew pulses.
// One pulse is 1/314.96 mm = 3.2 um of thread-flank error; the observed error
// across the speed sweep is ~1.3 pulses.
constexpr double kHelixTol = 2.0;
// A deliberately-wrong helix in these tests is offset by half a pitch
// (200 pulses); anything beyond this is unambiguously "the other helix".
constexpr double kOffHelixMin = 100.0;

// Signed distance from the carriage to the nearest point of the helix anchored
// at (anchorPos, anchorPhase), in LEADSCREW PULSES. Zero == on the helix.
//
// On-helix means (L - anchorPos)/ratio == (phase - anchorPhase)  (mod PPR).
// The residual is reduced into (-PPR/2, PPR/2] spindle pulses, then scaled back
// into leadscrew pulses so the tolerance is expressible as machine error.
double helixErrorPulses(int carriagePos, int spindlePhase, int anchorPos,
                        int anchorPhase, double ratio, int ppr) {
  double travelInSpindlePulses = (double)(carriagePos - anchorPos) / ratio;
  double e = travelInSpindlePulses - (double)(spindlePhase - anchorPhase);
  e = std::fmod(e, (double)ppr);
  if (e > ppr / 2.0) e -= ppr;
  if (e <= -ppr / 2.0) e += ppr;
  return e * ratio;
}

class SyncPointTest : public ::testing::Test {
 protected:
  LatheConfig cfg;
  LatheConfigDerived *derived = nullptr;
  Spindle *spindle = nullptr;
  LeadscrewIOMock io;
  Leadscrew *ls = nullptr;
  GlobalState *gs = nullptr;

  void SetUp() override {
    cfg = LatheConfig();
    cfg.check = CHECKVALUE;
    derived = new LatheConfigDerived(&cfg);
    gs = GlobalState::getInstance();
    buildRig();
  }

  void TearDown() override {
    delete ls;
    ls = nullptr;
    delete spindle;
    delete derived;
  }

  int encoderPPR() const { return derived->spindleEncoderPpr(); }
  int motorPPR() const {
    return derived->stepperPpr() * derived->gearboxRatio();
  }
  double ratioFor(float pitch) const {
    return ((double)pitch * (double)motorPPR()) /
           ((double)derived->leadscrewPitchMm() * (double)encoderPPR());
  }

  // (Re)build spindle + leadscrew in a fully known baseline. Called per speed so
  // each sweep point is isolated (including the spindle's velocity history).
  void buildRig() {
    delete ls;
    ls = nullptr;
    delete spindle;
    spindle = nullptr;
    resetMockClock();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    spindle = new Spindle(0, 1, derived);
    ls = new Leadscrew(derived, spindle, &io, derived->accellerationPulseSec(),
                       derived->leadscrewInitialPulseDelay(), motorPPR(),
                       derived->leadscrewPitchMm(), encoderPPR());
    ls->setTargetPitchMM(kPitchMm);
  }

  // Turn the spindle by `pulses` (signed) NET encoder counts at |pps|, one kDt
  // step at a time, calling ls->update() each step. Fractional pulses are
  // carried between steps so the average rate is exactly |pps|.
  void driveSpindle(int pulses, int pps) {
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

  // Hold the spindle stationary and pump update() until the carriage rests.
  void settle(int maxSteps = 40000) {
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

  // Hand-position the tool: put the carriage at `pulses` with the axis
  // disengaged, then pump one update() so the expected position is re-zeroed
  // onto it (MM_DISABLED + speed 0 does that) and no phantom position error is
  // left behind. Leadscrew has no independent carriage sensor, so this is the
  // faithful model of jogging/hand-cranking the tool into the groove.
  void placeCarriage(int pulses) {
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    ls->setCurrentPosition(pulses);
    advanceMockMicros(kDt);
    ls->update();
  }

  // Turn the spindle (by hand, disengaged) to a specific phase.
  void turnSpindleToPhase(int phase, int pps) {
    int delta = positiveModulo(phase - spindle->getCurrentPosition(),
                               encoderPPR());
    driveSpindle(delta, pps);
  }

  // The spindle phase at which carriage position `L` lies on the helix anchored
  // at (anchorPos, anchorPhase).
  int onHelixPhase(int L, int anchorPos, int anchorPhase, double ratio) const {
    return positiveModulo(
        (int)std::lround((double)(L - anchorPos) / ratio) + anchorPhase,
        encoderPPR());
  }

  // Break sync the way ending a pass does (HALT / endstop -> MM_DISABLED, which
  // makes update() flag SS_UNSYNC once the axis is at rest), turn the spindle
  // `spindleOffset` counts with the leadscrew frozen, then re-engage and run
  // `spindlePulses` further counts.
  //
  // This is the ONLY way to observe which helix is anchored - see the file
  // header. With a correct anchor the axis stalls on re-engagement until the
  // spindle phase brings the carriage back onto the anchored helix.
  void disengageAndReEngage(int spindleOffset, int spindlePulses, int pps) {
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    settle();
    // NB: the thread-sync FLAG is deliberately not asserted here. Coming to rest
    // while disabled drops it to SS_UNSYNC, but if the carriage happens to be
    // sitting exactly on the anchored helix at the spindle's current angle the
    // re-sync block in the same update() immediately raises it again - which is
    // correct, it really is in sync. The durable thing is the ANCHOR, and the
    // gate is re-armed either way as soon as the spindle moves off phase.

    const int frozenAt = ls->getCurrentPosition();
    driveSpindle(spindleOffset, pps);
    ASSERT_EQ(ls->getCurrentPosition(), frozenAt)
        << "carriage moved while the axis was disengaged";

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(spindlePulses, pps);
    settle();
  }
};

// ---------------------------------------------------------------------------
// Sanity: the numbers this suite reasons about really are what the config gives.
// ---------------------------------------------------------------------------

TEST_F(SyncPointTest, SanityConfigGivesTheExpectedHelixGeometry) {
  ASSERT_EQ(encoderPPR(), kPPR);
  ASSERT_EQ(motorPPR(), 800);
  ASSERT_NEAR(ratioFor(kPitchMm), 1.0 / 3.0, 1e-9);
  ASSERT_NEAR(ratioFor(kPitchMm) * encoderPPR(), kPulsesPerPitch, 1e-6);
}

// ---------------------------------------------------------------------------
// State contract
// ---------------------------------------------------------------------------

// The declaration itself: syncing marks the thread as synced. At the instant of
// the call the carriage is, by definition, on the helix.
TEST_F(SyncPointTest, SetSyncPointMarksTheThreadSynced) {
  gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
  placeCarriage(1234);
  turnSpindleToPhase(377, 300);

  ls->setSyncPoint();

  EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
}

// It works with NO stops set - you are picking up a thread, not running between
// endstops - and it must not invent stops to hold the anchor in. (A tempting
// wrong implementation is to stuff the carriage position into leftStopPosition
// and set syncPositionState to LEFT; that would silently arm a left endstop
// under the user's feet.)
TEST_F(SyncPointTest, SetSyncPointWithNoStopsCreatesNoStops) {
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::UNSET);

  placeCarriage(500);
  turnSpindleToPhase(900, 300);
  ls->setSyncPoint();

  EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::UNSET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), INT32_MIN);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::RIGHT), INT32_MAX);
}

// ...and it must not disturb stops that ARE set. Picking the thread up again
// mid-job must not cost the user their carefully-set shoulder.
TEST_F(SyncPointTest, SetSyncPointLeavesExistingStopsAlone) {
  placeCarriage(0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -500);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 900);

  placeCarriage(120);
  turnSpindleToPhase(640, 300);
  ls->setSyncPoint();

  EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::SET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), -500);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::SET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::RIGHT), 900);
}

// ---------------------------------------------------------------------------
// The headline property: the carriage returns to the sync position at every
// whole multiple of the pitch, at every speed.
// ---------------------------------------------------------------------------

TEST_F(SyncPointTest, CarriageReturnsToSyncPositionEveryWholePitch) {
  const double ratio = ratioFor(kPitchMm);
  const int L0 = 700;  // sync taken somewhere arbitrary, NOT at zero

  std::vector<int> travels;
  for (int pps : kSpeeds) {
    buildRig();
    placeCarriage(L0);
    turnSpindleToPhase(413, pps);
    const int phi0 = spindle->getCurrentPosition();

    ls->setSyncPoint();
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC)
        << "setSyncPoint() did not establish sync at " << pps << " PPS";

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    for (int rev = 1; rev <= 3; ++rev) {
      driveSpindle(encoderPPR(), pps);  // exactly one more spindle revolution
      settle();

      // A whole revolution later the spindle is back at the sync ANGLE...
      EXPECT_EQ(spindle->getCurrentPosition(), phi0)
          << "spindle phase drifted after " << rev << " rev(s)";
      // ...and the carriage is exactly one more whole PITCH along, i.e. back at
      // the same point of the thread groove.
      EXPECT_NEAR(ls->getCurrentPosition(), L0 + rev * kPulsesPerPitch,
                  kTravelTol)
          << "carriage is not " << rev << " whole pitch(es) from the sync point"
          << " at " << pps << " PPS";
      EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                   spindle->getCurrentPosition(), L0, phi0,
                                   ratio, encoderPPR()),
                  0.0, kHelixTol)
          << "off the anchored helix after " << rev << " rev(s) at " << pps
          << " PPS";
    }

    // Wind back to the sync point: the tool must land exactly where it started.
    driveSpindle(-3 * encoderPPR(), pps);
    settle();
    EXPECT_NEAR(ls->getCurrentPosition(), L0, kTravelTol)
        << "did not return to the sync position at " << pps << " PPS";
    travels.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 1; i < travels.size(); ++i) {
    EXPECT_NEAR(travels[i], travels[0], 1) << "result differs across speeds";
  }
}

// Mirror for a LEFT-HAND / reverse thread (negative pitch, as FM_THREAD_REVERSE
// feeds via a negated ratio). Same helix, opposite travel direction.
TEST_F(SyncPointTest, ReverseThreadCarriageReturnsEveryWholePitch) {
  const double ratio = ratioFor(-kPitchMm);
  const int L0 = -300;

  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(-kPitchMm);
    placeCarriage(L0);
    turnSpindleToPhase(413, pps);
    const int phi0 = spindle->getCurrentPosition();

    ls->setSyncPoint();
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC)
        << "setSyncPoint() did not establish sync at " << pps << " PPS";

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    for (int rev = 1; rev <= 3; ++rev) {
      driveSpindle(encoderPPR(), pps);
      settle();
      EXPECT_EQ(spindle->getCurrentPosition(), phi0);
      EXPECT_NEAR(ls->getCurrentPosition(), L0 - rev * kPulsesPerPitch,
                  kTravelTol)
          << "reverse thread: not " << rev << " whole pitch(es) from the sync"
          << " point at " << pps << " PPS";
      EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                   spindle->getCurrentPosition(), L0, phi0,
                                   ratio, encoderPPR()),
                  0.0, kHelixTol)
          << "reverse thread off the anchored helix at " << pps << " PPS";
    }
  }
}

// ---------------------------------------------------------------------------
// The anchor is a real anchor: it survives disengagement and it is taken from
// the CURRENT spindle angle / carriage position, not from a stop.
// ---------------------------------------------------------------------------

// The whole point of the feature. Sync, cut a pass, disengage, let the spindle
// turn a fractional revolution while the tool is clear (exactly what happens
// when you back the tool out and wind the chuck round by hand), then re-engage:
// the axis must WAIT for the groove to come round rather than starting wherever
// it happens to be.
TEST_F(SyncPointTest, HelixSurvivesDisengagementAndAFractionalTurn) {
  const double ratio = ratioFor(kPitchMm);
  const int L0 = 0;

  for (int pps : kSpeeds) {
    buildRig();
    placeCarriage(L0);
    turnSpindleToPhase(500, pps);
    const int phi0 = spindle->getCurrentPosition();
    ls->setSyncPoint();
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(2 * encoderPPR(), pps);  // cut a pass
    settle();

    // Disengage, turn the spindle 137 counts (a deliberately non-integral
    // fraction of a revolution) with the tool clear, re-engage, cut on.
    disengageAndReEngage(/*spindleOffset=*/137, /*spindlePulses=*/3 * encoderPPR(),
                         pps);

    EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                 spindle->getCurrentPosition(), L0, phi0, ratio,
                                 encoderPPR()),
                0.0, kHelixTol)
        << "re-engaged onto a DIFFERENT helix at " << pps << " PPS - the tool "
        << "would cut a new groove across the old one";
  }
}

// The same, for a LEFT-HAND / reverse thread (negative pitch, hence a negative
// ratio and a negative divisor in the gate's phase computation).
//
// ReverseThreadCarriageReturnsEveryWholePitch above does NOT cover this: it
// never disengages, so - exactly as the file header warns - it only exercises
// the relative feed and cannot tell a correct anchor from a wrong one. (Proved
// by mutation: inverting the sign of the MANUAL arm leaves that test green.)
// Since a negative ratio is the case most likely to be subtly wrong in the sign
// or origin of the anchor, it needs a test that actually gates on the anchor.
TEST_F(SyncPointTest, ReverseThreadHelixSurvivesDisengagement) {
  const double ratio = ratioFor(-kPitchMm);
  const int L0 = -300;

  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(-kPitchMm);
    placeCarriage(L0);
    turnSpindleToPhase(500, pps);
    const int phi0 = spindle->getCurrentPosition();
    ls->setSyncPoint();
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(2 * encoderPPR(), pps);  // cut a pass
    settle();

    disengageAndReEngage(/*spindleOffset=*/137,
                         /*spindlePulses=*/3 * encoderPPR(), pps);

    EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                 spindle->getCurrentPosition(), L0, phi0, ratio,
                                 encoderPPR()),
                0.0, kHelixTol)
        << "left-hand thread re-engaged onto a DIFFERENT helix at " << pps
        << " PPS";
  }
}

// A sync taken with the spindle dead stopped must still hold once it spins up:
// the anchor is an angle + a position, not anything speed-derived.
TEST_F(SyncPointTest, SyncTakenAtRestHoldsOnceTheSpindleSpinsUp) {
  const double ratio = ratioFor(kPitchMm);
  const int L0 = 250;

  std::vector<int> finals;
  for (int pps : kSpeeds) {
    buildRig();
    placeCarriage(L0);

    // Spindle genuinely at rest: pump the loop for 50 ms of virtual time with
    // no encoder motion at all, so the velocity estimate is a hard zero.
    for (int i = 0; i < 500; ++i) {
      advanceMockMicros(kDt);
      ls->update();
    }
    const int phi0 = spindle->getCurrentPosition();
    ASSERT_FLOAT_EQ(spindle->getEstimatedVelocityInPPS(), 0.0f)
        << "spindle was not actually at rest when the sync was taken";

    ls->setSyncPoint();
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

    // Now spin up and cut. The 500-count nudge takes the spindle off the sync
    // angle first, so the re-sync gate has real work to do: an anchor captured
    // from a dead-stopped spindle has to bring the axis onto the helix at speed.
    disengageAndReEngage(/*spindleOffset=*/500,
                         /*spindlePulses=*/3 * encoderPPR(), pps);

    EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                 spindle->getCurrentPosition(), L0, phi0, ratio,
                                 encoderPPR()),
                0.0, kHelixTol)
        << "sync taken at rest did not hold at " << pps << " PPS";
    finals.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 1; i < finals.size(); ++i) {
    EXPECT_NEAR(finals[i], finals[0], kTravelTol)
        << "the anchored helix is speed-dependent (" << kSpeeds[i] << " vs "
        << kSpeeds[0] << " PPS)";
  }
}

// The anchor comes from the CURRENT spindle angle and carriage position - NOT
// from a stop, and not from an anchor a stop latched earlier. Here a stop-
// derived anchor already exists; the user then finds the real groove half a
// pitch away and declares sync there. The explicit declaration must win.
TEST_F(SyncPointTest, AnchorIsTakenFromTheCurrentPositionNotFromAStop) {
  const double ratio = ratioFor(kPitchMm);
  const int pps = 1200;

  placeCarriage(0);
  turnSpindleToPhase(100, pps);
  // Incidental, stop-derived anchor at (0, 100).
  ls->setStopPosition(LeadscrewStopPosition::LEFT);
  const int stopAnchorPos = 0;
  const int stopAnchorPhase = spindle->getCurrentPosition();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  // Hand-position the tool in the real groove: a carriage position and a
  // spindle angle that are deliberately HALF A PITCH off the stop's helix.
  const int L0 = 640;
  placeCarriage(L0);
  turnSpindleToPhase(positiveModulo(onHelixPhase(L0, stopAnchorPos,
                                                 stopAnchorPhase, ratio) +
                                        encoderPPR() / 2,
                                    encoderPPR()),
                     pps);
  const int phi0 = spindle->getCurrentPosition();
  ASSERT_NEAR(std::fabs(helixErrorPulses(L0, phi0, stopAnchorPos,
                                         stopAnchorPhase, ratio, encoderPPR())),
              kPulsesPerPitch / 2.0, 1.0)
      << "test setup: the new sync point is not half a pitch off the stop helix";

  ls->setSyncPoint();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(2 * encoderPPR(), pps);
  settle();
  disengageAndReEngage(/*spindleOffset=*/311, /*spindlePulses=*/3 * encoderPPR(),
                       pps);

  const int L = ls->getCurrentPosition();
  const int phase = spindle->getCurrentPosition();
  EXPECT_NEAR(helixErrorPulses(L, phase, L0, phi0, ratio, encoderPPR()), 0.0,
              kHelixTol)
      << "not on the helix declared by setSyncPoint()";
  EXPECT_GT(std::fabs(helixErrorPulses(L, phase, stopAnchorPos, stopAnchorPhase,
                                       ratio, encoderPPR())),
            kOffHelixMin)
      << "still riding the stop-derived helix - setSyncPoint() was ignored";
}

// Syncing twice: the second call wins outright. (You mis-set it, or you moved
// to a different groove on a multi-start thread.)
TEST_F(SyncPointTest, SecondSyncPointWins) {
  const double ratio = ratioFor(kPitchMm);
  const int pps = 1200;

  const int firstPos = 0;
  placeCarriage(firstPos);
  turnSpindleToPhase(200, pps);
  const int firstPhase = spindle->getCurrentPosition();
  ls->setSyncPoint();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  // Second sync, half a pitch off the first helix.
  const int secondPos = 530;
  placeCarriage(secondPos);
  turnSpindleToPhase(
      positiveModulo(
          onHelixPhase(secondPos, firstPos, firstPhase, ratio) + encoderPPR() / 2,
          encoderPPR()),
      pps);
  const int secondPhase = spindle->getCurrentPosition();
  ls->setSyncPoint();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(2 * encoderPPR(), pps);
  settle();
  disengageAndReEngage(/*spindleOffset=*/689, /*spindlePulses=*/3 * encoderPPR(),
                       pps);

  const int L = ls->getCurrentPosition();
  const int phase = spindle->getCurrentPosition();
  EXPECT_NEAR(helixErrorPulses(L, phase, secondPos, secondPhase, ratio,
                               encoderPPR()),
              0.0, kHelixTol)
      << "not on the helix of the SECOND setSyncPoint()";
  EXPECT_GT(std::fabs(
                helixErrorPulses(L, phase, firstPos, firstPhase, ratio,
                                 encoderPPR())),
            kOffHelixMin)
      << "still on the first helix - the second setSyncPoint() was ignored";
}

// ---------------------------------------------------------------------------
// Interaction with the stop machinery.
//
// DECISION: an explicit sync OUTRANKS an incidental stop-derived anchor, and
// survives unsetStop() completely untouched - neither dropped nor migrated onto
// the surviving stop. Rationale in the report and in leadscrew.h; the short
// version is that unsetStop()'s re-anchor exists only because a stop-derived
// anchor stores its carriage coordinate AS the stop, so removing the stop
// destroys it. A manual sync owns its own coordinate, so there is nothing to
// migrate - and dropping it is the dangerous direction: syncArmed() would go
// false, update()'s phase gate would disappear entirely, and the next
// engagement would cut a fresh groove at an arbitrary angle. That is precisely
// the failure this feature exists to prevent.
// ---------------------------------------------------------------------------

// Setting a stop AFTER an explicit sync must not steal the anchor, even when the
// stop lands exactly on the current carriage position (the condition under which
// setStop() normally latches one).
TEST_F(SyncPointTest, SettingAStopAfterwardsDoesNotStealTheAnchor) {
  const double ratio = ratioFor(kPitchMm);
  const int pps = 1200;
  const int L0 = 160;

  placeCarriage(L0);
  turnSpindleToPhase(820, pps);
  const int phi0 = spindle->getCurrentPosition();
  ls->setSyncPoint();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  // Turn the spindle half a pitch's worth without moving, then drop a stop right
  // here. A stolen anchor would be (L0, phi0 + 600) - a helix half a pitch out.
  driveSpindle(encoderPPR() / 2, pps);
  const int stolenPhase = spindle->getCurrentPosition();
  ls->setStopPosition(LeadscrewStopPosition::LEFT);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::SET);
  EXPECT_EQ(ls->getStopPosition(LeadscrewStopPosition::LEFT), L0);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(2 * encoderPPR(), pps);
  settle();
  disengageAndReEngage(/*spindleOffset=*/451, /*spindlePulses=*/3 * encoderPPR(),
                       pps);

  const int L = ls->getCurrentPosition();
  const int phase = spindle->getCurrentPosition();
  EXPECT_NEAR(helixErrorPulses(L, phase, L0, phi0, ratio, encoderPPR()), 0.0,
              kHelixTol)
      << "setStopPosition() overwrote the explicit sync anchor";
  EXPECT_GT(
      std::fabs(helixErrorPulses(L, phase, L0, stolenPhase, ratio, encoderPPR())),
      kOffHelixMin)
      << "riding the helix the stop would have latched";
}

// THE DECISION, pinned: clearing a stop must not disturb an explicit sync -
// not even when the other stop is still set, which is the case where
// unsetStop() MIGRATES a stop-derived anchor across the gap.
TEST_F(SyncPointTest, ExplicitSyncSurvivesUnsetStopWithTheOtherStopStillSet) {
  const double ratio = ratioFor(kPitchMm);
  const int pps = 1200;

  // A stop-derived anchor exists first (left stop at the current position), and
  // a right stop exists for unsetStop() to try to migrate onto.
  placeCarriage(0);
  turnSpindleToPhase(75, pps);
  ls->setStopPosition(LeadscrewStopPosition::LEFT);
  // Far enough right that this run never reaches it: the point of the right stop
  // here is only that it EXISTS, so unsetStop() has somewhere to migrate a
  // stop-derived anchor to. Parking on it would end the pass off-helix and mask
  // what is being measured.
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 5000);
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  // The user then finds the real groove and declares sync half a pitch off.
  const int L0 = 480;
  placeCarriage(L0);
  turnSpindleToPhase(positiveModulo(onHelixPhase(L0, 0, 75, ratio) +
                                        encoderPPR() / 2,
                                    encoderPPR()),
                     pps);
  const int phi0 = spindle->getCurrentPosition();
  ls->setSyncPoint();
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  // Clear the left stop. Under the stop-derived rules this would migrate the
  // anchor onto the right stop (extrapolating the phase across 1600 pulses);
  // with an explicit sync there is nothing to migrate.
  ls->unsetStopPosition(LeadscrewStopPosition::LEFT);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  EXPECT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::SET);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(2 * encoderPPR(), pps);
  settle();
  disengageAndReEngage(/*spindleOffset=*/233, /*spindlePulses=*/3 * encoderPPR(),
                       pps);

  EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                               spindle->getCurrentPosition(), L0, phi0, ratio,
                               encoderPPR()),
              0.0, kHelixTol)
      << "unsetStop() disturbed the explicit sync anchor";
}

// ...and clearing the LAST stop must not drop it either. This is the dangerous
// case: today unsetStop() with no surviving stop sets syncPositionState to UNSET,
// which makes syncArmed() false and REMOVES update()'s phase gate altogether -
// the axis would then engage at whatever angle the spindle happened to be at.
TEST_F(SyncPointTest, ExplicitSyncSurvivesClearingTheLastStop) {
  const double ratio = ratioFor(kPitchMm);
  const int pps = 1200;

  placeCarriage(0);
  turnSpindleToPhase(75, pps);
  ls->setStopPosition(LeadscrewStopPosition::LEFT);  // incidental anchor
  ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);

  const int L0 = 480;
  placeCarriage(L0);
  turnSpindleToPhase(
      positiveModulo(onHelixPhase(L0, 0, 75, ratio) + encoderPPR() / 2,
                     encoderPPR()),
      pps);
  const int phi0 = spindle->getCurrentPosition();
  ls->setSyncPoint();

  ls->unsetStopPosition(LeadscrewStopPosition::LEFT);  // no stops left at all
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::LEFT),
            LeadscrewStopState::UNSET);
  ASSERT_EQ(ls->getStopPositionState(LeadscrewStopPosition::RIGHT),
            LeadscrewStopState::UNSET);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(2 * encoderPPR(), pps);
  settle();
  disengageAndReEngage(/*spindleOffset=*/233, /*spindlePulses=*/3 * encoderPPR(),
                       pps);

  EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                               spindle->getCurrentPosition(), L0, phi0, ratio,
                               encoderPPR()),
              0.0, kHelixTol)
      << "clearing the last stop dropped the explicit sync anchor - the phase "
      << "gate is gone and the tool re-engaged at an arbitrary angle";
}

// ---------------------------------------------------------------------------
// Taking a sync while the re-sync gate is HOLDING the axis.
//
// This is the case that makes setSyncPoint() unable to raise SS_SYNC itself.
// While the gate holds (syncArmed() && SS_UNSYNC) update() keeps accumulating
// spindle motion into m_expectedPosition with the carriage frozen, so the
// following error grows - up to a whole pitch over one spindle revolution.
// Firing the gate is what DISCARDS that accumulation. Anything that flags the
// thread synced without going through the gate releases the axis with the error
// still in it, and it closes that error at maximum speed: a lurch of up to
// 1.27 mm straight into the work.
// ---------------------------------------------------------------------------

TEST_F(SyncPointTest, SyncTakenWhileTheGateIsHoldingDoesNotLurch) {
  const double ratio = ratioFor(kPitchMm);
  const int L0 = 0;

  for (int pps : kSpeeds) {
    buildRig();
    placeCarriage(L0);
    turnSpindleToPhase(0, pps);
    ls->setSyncPoint();

    // Disengage, come to rest, then take the spindle half a revolution off the
    // sync angle so the carriage at L0 is decidedly NOT on the anchored helix.
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    settle();
    driveSpindle(encoderPPR() / 2, pps);
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_UNSYNC);
    const int frozenAt = ls->getCurrentPosition();
    ASSERT_EQ(frozenAt, L0);

    // Engage. The gate must hold the axis while the spindle closes on the sync
    // angle - and while it holds, m_expectedPosition silently runs away from the
    // frozen carriage. A quarter revolution is short enough to stay clear of the
    // point at which the gate fires of its own accord: it fires EARLY, by the
    // acceleration lead pulsesToTargetSpeed, and that lead grows with speed.
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(encoderPPR() / 4, pps);
    ASSERT_EQ(ls->getCurrentPosition(), frozenAt)
        << "the gate did not hold the axis at " << pps << " PPS";
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_UNSYNC);
    ASSERT_GT(std::fabs(ls->getPositionError()), 100.0f)
        << "test setup: no following error accumulated, so this test would "
           "pass vacuously";

    // The operator declares sync HERE. The carriage has not moved and is, by
    // that declaration, in the groove - so it must STAY WHERE IT IS. The
    // accumulated error is not travel that is owed to anybody; it is the
    // bookkeeping of a hold, and declaring sync must discard it.
    const int phi0 = spindle->getCurrentPosition();
    ls->setSyncPoint();
    settle();

    EXPECT_NEAR(ls->getCurrentPosition(), frozenAt, kTravelTol)
        << "the carriage lurched " << (ls->getCurrentPosition() - frozenAt)
        << " pulses when sync was taken at " << pps << " PPS - the following "
        << "error accumulated while the gate was holding was never discarded";
    EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC)
        << "the gate did not pick up the freshly declared anchor";

    // And the anchor that took effect is the declared one: cut on, and the tool
    // must ride the helix through (frozenAt, phi0).
    driveSpindle(3 * encoderPPR(), pps);
    settle();
    EXPECT_NEAR(helixErrorPulses(ls->getCurrentPosition(),
                                 spindle->getCurrentPosition(), frozenAt, phi0,
                                 ratio, encoderPPR()),
                0.0, kHelixTol)
        << "not on the helix declared mid-hold at " << pps << " PPS";
  }
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
