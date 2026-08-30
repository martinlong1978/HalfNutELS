// Can the stop BEHIND the carriage decelerate a thread pass mid-flight?
//
// THE HYPOTHESIS THIS SUITE WAS WRITTEN TO TEST, and it does not survive.
//
// The two "going to hit" predicates in lib/leadscrew/leadscrew_stopsync.h are
// direction-agnostic - they are pure proximity tests and carry no term for
// which way the carriage is travelling:
//
//     goingToHitLeftEndstop (pos, d) = leftSet  && pos - d <= leftStopPosition
//     goingToHitRightEndstop(pos, d) = rightSet && pos + d >= rightStopPosition
//
// Leadscrew::update() ORs both into `shouldStop` in the MM_ENABLED arm, and
// feeds them d = getStoppingDistanceInPulses() = v^2 / (2a), which is QUADRATIC
// in the commanded speed. So on paper an axis feeding AWAY from a stop should
// be able to trip that stop's predicate - "I could not stop before the thing
// behind me" - and be forced into decelerationStep() mid-pass while the mode
// stays MM_ENABLED. The panel prints CUTTING for MM_ENABLED and for nothing
// else, so that would read as a cut stopped dead with the state word still
// saying CUTTING: the exact signature of the field report this came from.
//
// VERDICT: REFUTED as a cause of that report, for a reason that turns out to be
// structural rather than a matter of margin.
//
//   The axis can only reach speed v at distance s from where it last stood
//   still by accelerating, so v <= sqrt(2 a s), so its stopping distance
//   v^2/(2a) is at most s. A stop can only ever be PLACED at a position the
//   carriage is standing on (setStopPosition() with no coordinate takes
//   m_currentPosition, and UiState refuses every stop edit while the carriage
//   is under power - stopEditsInhibited()/the motion lockout, lib/ui/uistate.cpp),
//   so the point the axis last stood still at is never on the far side of the
//   stop behind it. Therefore
//
//       pos - stoppingDistance  >=  restPosition  >=  stopPosition
//
//   and the rear predicate is false BY CONSTRUCTION, not by luck. The pass and
//   the predicate are two readings of the same acceleration ramp, so they sit
//   exactly on top of one another and the predicate can only be tipped over by
//   float rounding at the boundary.
//
// Measured, over pitch {0.25, 1.0, 3.0} mm x spindle {300, 1200, 3000} PPS
// (PPR 1200, so 15/60/150 rpm) on the default config: the rear predicate is
// true on ZERO of the pulses of every one of those nine passes, and the whole
// carriage-position trace is bit-identical to the same pass run with the rear
// stop not set at all. Pushed off the end of the realistic sweep to 12000 PPS
// (600 rpm) it becomes true exactly ONCE per pass, 46 pulses (0.15 mm) off the
// stop, and costs 3 pulses of travel out of 716. That is the whole effect.
//
// AND WHEN IT IS FORCED TO FIRE, IT IS A TRANSIENT, NOT A STALL. Planting a
// stop 10 pulses behind a carriage that is ALREADY at speed (not reachable from
// the panel - see above - but it is the only way to get the predicate to bite,
// and it is the severity question worth answering) does decelerate the axis and
// does cost tracking accuracy, but it always escapes: distance grows while the
// commanded speed falls, so the quadratic term collapses. The carriage never
// stops, the travel over the run is unchanged, and the mode never leaves
// MM_ENABLED-with-motion. See PlantedRearStopDeceleratesButDoesNotStall for the
// numbers.
//
// SO THE HYPOTHESIS'S FAILURE MODE - carriage stationary, mode MM_ENABLED,
// panel reading CUTTING - IS NOT PRODUCIBLE THIS WAY, at any speed or pitch
// tried. Nothing here justifies changing the predicates.
//
// WHAT DID TURN UP, and it is a real defect: a pass started from its stop at a
// low enough feed never starts at all - the axis dithers, walks back onto the
// stop and arrests itself. That is the direction latch and the endstop arrest,
// NOT these predicates (the same run with the stop one pulse further back
// completes normally), so it is a cousin of test/test_endstop_deadlock rather
// than of anything above. It is asserted as a defect at the bottom of this
// file; the note there explains why it is nonetheless NOT the field report
// either.
//
// A NOTE ON THE DEMO BRANCH. The report's prime suspect was
// demo/ep2-pre-sync-fix, whose HEAD (80e7545, "DEMO ONLY") drops the
// pulsesToTargetSpeed lead term from the direction and sync-gate decisions in
// Leadscrew::update() to reproduce the pre-c3db8cd over-speeding sync-start.
// Every result in this file was taken TWICE, once with that commit applied and
// once without, and the two runs are indistinguishable: same pass/fail set,
// same trip counts, same dither threshold, same carriage positions. The demo
// commit does not touch the stop predicates or getStoppingDistanceInPulses(),
// and - the point - it cannot manufacture the one thing the hypothesis needs,
// a carriage moving faster than it could have accelerated to from the stop
// behind it. So "it was probably running the demo branch" does not rescue the
// hypothesis either.
#include <gmock/gmock.h>

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "globalstate.h"
#include "latheconfig.h"
#include "leadscrew.h"
#include "leadscrew_stopsync.h"
#include "leadscrewio_mock.h"
#include "spindle.h"

namespace {

// Simulation step: 100 us of virtual time per update(), as test_thread_sync.
constexpr uint64_t kDt = 100;

// The realistic sweep, straight from test_thread_sync: encoderPPR is 1200, so
// these are 15 / 60 / 150 rpm.
const int kSpeeds[] = {300, 1200, 3000};

// ===========================================================================
// PART 1 - the predicate on its own.
//
// LeadscrewStopSync is a plain struct of plain fields with header-inline
// predicates, so these need no clock, no spindle and no motion: they are
// arithmetic. They exist to state exactly WHAT the predicate answers, so that
// Part 2 can be about whether that answer is ever reached rather than about
// what it means.
// ===========================================================================

// CHARACTERIZATION. The predicate takes no direction argument and has no
// direction term, so the same (position, stopping distance) pair gives the same
// answer for a carriage feeding away from the stop as for one feeding into it.
// This is the property the hypothesis is built on; it is recorded here as fact,
// not asserted as a defect - Part 2 is where it is judged, and the judgement is
// that the caller never reaches the state where it matters.
TEST(RearStopPredicate, IgnoresTravelDirection) {
  LeadscrewStopSync s;
  s.setStop(LeadscrewStopPosition::LEFT, 0, 0, 0);

  // 500 pulses to the right of the left stop, planning to stop in 600. The
  // predicate says "yes, going to hit it" - and it would say the same whether
  // this carriage were retreating from that stop or reversing into it, because
  // nothing in the expression can tell those apart.
  EXPECT_TRUE(s.goingToHitLeftEndstop(500, 600));

  // The mirror, for the right stop and a carriage sitting to its left.
  LeadscrewStopSync t;
  t.setStop(LeadscrewStopPosition::RIGHT, 0, 0, 0);
  EXPECT_TRUE(t.goingToHitRightEndstop(-500, 600));
}

// CHARACTERIZATION. The exact threshold, since everything in Part 2 turns on
// it: the rear predicate flips the instant the planned stopping distance
// reaches the distance already travelled from the stop. Not one pulse before.
TEST(RearStopPredicate, FlipsWhenTheStoppingDistanceReachesTheDistanceTravelled) {
  LeadscrewStopSync s;
  s.setStop(LeadscrewStopPosition::LEFT, 0, 0, 0);

  EXPECT_FALSE(s.goingToHitLeftEndstop(500, 499));
  EXPECT_TRUE(s.goingToHitLeftEndstop(500, 500));
  EXPECT_TRUE(s.goingToHitLeftEndstop(500, 501));

  // Sitting on the stop, it is true for any stopping distance at all,
  // including zero - which is just hitLeftEndstop() said a second way.
  EXPECT_TRUE(s.goingToHitLeftEndstop(0, 0));
}

// An unset stop is never approached, whatever the numbers say. Pinned because
// the sentinel positions (INT32_MIN / INT32_MAX) would otherwise make both
// predicates permanently true or permanently false by accident.
TEST(RearStopPredicate, AnUnsetStopIsNeverGoingToBeHit) {
  LeadscrewStopSync s;
  EXPECT_FALSE(s.goingToHitLeftEndstop(500, 100000));
  EXPECT_FALSE(s.goingToHitRightEndstop(-500, 100000));
}

// ===========================================================================
// PART 2 - the predicate through Leadscrew::update().
// ===========================================================================

struct PassRun {
  std::vector<int> trace;   // carriage position after every update()
  int finalPos = 0;
  int mode = 0;
  float maxErr = 0.0f;
  float maxSpeed = 0.0f;
  long rearTrips = 0;       // pulses on which the rear predicate was true
  long pulses = 0;          // pulses actually emitted
  int maxTripDistance = -1; // furthest from the rear stop it still tripped
  int longestStill = 0;     // longest run of iterations with no carriage motion
};

class MidPassStallTest : public ::testing::Test {
 protected:
  LatheConfig cfg;
  LatheConfigDerived* derived = nullptr;
  Spindle* spindle = nullptr;
  LeadscrewIOMock io;
  Leadscrew* ls = nullptr;
  GlobalState* gs = nullptr;

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
                       derived->leadscrewInitialPulseDelay(),
                       derived->stepperPpr() * derived->gearboxRatio(),
                       derived->leadscrewPitchMm(), encoderPPR());
  }

  float ratioFor(float pitch) const {
    return (pitch * (float)(derived->stepperPpr() * derived->gearboxRatio())) /
           (derived->leadscrewPitchMm() * (float)encoderPPR());
  }

  // Reconstruct the rear predicate exactly as update() evaluates it.
  //
  // THE TWO SAMPLING POINTS MATTER AND ARE EASY TO GET WRONG - getting them
  // wrong is what made the first draft of this suite report thousands of
  // spurious trips. update() computes pulsesToStop from m_leadscrewSpeed as it
  // stands BEFORE this iteration's accel/decel step, and compares it against
  // m_currentPosition AFTER the pulse has been counted. And the whole block
  // lives inside `if (sendPulse())`, so it does not run on an iteration that
  // emitted no pulse. Sampling the speed after update() instead of before
  // over-reads it by one full acceleration step, which is exactly enough to
  // flip a predicate that - as Part 2 shows - sits precisely on its boundary.
  bool rearPredicateFired(float speedBeforeUpdate, int positionAfterUpdate,
                          int rearStopPosition) const {
    const float v = speedBeforeUpdate;
    const int pulsesToStop = (int)(v * (v / derived->accellerationPulseSec()) / 2);
    return positionAfterUpdate - pulsesToStop <= rearStopPosition;
  }

  // A thread pass. The carriage starts at 0; `rearStopAt` places the stop it is
  // feeding away from (INT32_MIN leaves it unset - the control arm), `farStopAt`
  // the one it is feeding towards. Positive pitch + positive spindle = travel
  // right, per test_thread_sync's sign convention.
  //
  // The helix is anchored with setSyncPoint() rather than by the rear stop, in
  // BOTH arms, so that setting or not setting the rear stop is the only thing
  // that differs between them. Anchoring off the stop (the usual gesture) would
  // make the control arm unsyncable and the comparison meaningless.
  PassRun pass(float pitch, int pps, int rearStopAt, int farStopAt,
           int spindlePulses, bool keepTrace = false) {
    buildRig();
    ls->setTargetPitchMM(pitch);
    ls->setCurrentPosition(0);
    ls->setStopPosition(pitch >= 0 ? LeadscrewStopPosition::RIGHT
                                   : LeadscrewStopPosition::LEFT,
                        farStopAt);
    ls->setSyncPoint();
    if (rearStopAt != INT32_MIN) {
      ls->setStopPosition(pitch >= 0 ? LeadscrewStopPosition::LEFT
                                     : LeadscrewStopPosition::RIGHT,
                          rearStopAt);
    }
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    PassRun r;
    const double perStep = (double)pps * (double)kDt / 1e6;
    double carry = 0.0;
    int delivered = 0;
    int lastPos = 0;
    int still = 0;
    while (delivered < spindlePulses) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        if (delivered + whole > spindlePulses) whole = spindlePulses - delivered;
        carry -= (double)((int)carry);
        spindle->incrementCurrentPosition(whole);
        delivered += whole;
      }
      const float vBefore = ls->getLeadscrewSpeedPulsesPerSecond();
      const int posBefore = ls->getCurrentPosition();
      ls->update();
      const int pos = ls->getCurrentPosition();

      if (keepTrace) r.trace.push_back(pos);
      const float v = ls->getLeadscrewSpeedPulsesPerSecond();
      if (v > r.maxSpeed) r.maxSpeed = v;
      const float e = fabsf(ls->getPositionError());
      if (e > r.maxErr) r.maxErr = e;
      if (pos != posBefore) {
        r.pulses++;
        if (rearStopAt != INT32_MIN &&
            rearPredicateFired(vBefore, pos, rearStopAt)) {
          r.rearTrips++;
          const int d = abs(pos - rearStopAt);
          if (d > r.maxTripDistance) r.maxTripDistance = d;
        }
      }
      if (pos == lastPos) {
        if (++still > r.longestStill) r.longestStill = still;
      } else {
        still = 0;
        lastPos = pos;
      }
    }
    r.finalPos = ls->getCurrentPosition();
    r.mode = (int)gs->getMotionMode();
    return r;
  }
};

// The reach of the rear predicate, in the only units that decide whether it can
// ever matter: how far behind the carriage a stop has to be before the planned
// stopping distance can no longer touch it.
//
// Pinned because every "it cannot bite" conclusion below rests on these
// numbers, and they are all derived from the default LatheConfig - if the
// acceleration or the max speed is ever changed, this is the test that should
// make somebody re-read the rest of this file.
TEST_F(MidPassStallTest, TheReachOfTheRearPredicateIsQuadraticInSpeed) {
  const float a = derived->accellerationPulseSec();
  const float stepsPerMm = derived->leadscrewStepsPerMm();

  // Default config: 400 step/rev x 2:1 gearbox over a 2.54 mm leadscrew.
  ASSERT_NEAR(stepsPerMm, 314.96f, 0.01f);
  ASSERT_NEAR(a, 47244.1f, 1.0f);
  ASSERT_NEAR(derived->leadscrewMaxSpeedPps(), 12598.4f, 1.0f);

  auto reach = [a](float v) { return v * v / (2.0f * a); };

  // At the axis's absolute maximum the reach is 5.3 mm - the largest it can
  // ever be, and the number the hypothesis needs.
  EXPECT_NEAR(reach(derived->leadscrewMaxSpeedPps()) / stepsPerMm, 5.33f, 0.05f);

  // At the feeds a thread pass actually runs at it is a rounding error. 3 mm
  // pitch at 150 rpm - the fastest point of the realistic sweep - demands
  // 7.5 mm/s, and plans to stop within a fifth of a millimetre.
  const float fastestSweepFeedPps = 7.5f * stepsPerMm;
  EXPECT_NEAR(reach(fastestSweepFeedPps) / stepsPerMm, 0.19f, 0.01f);

  // 0.25 mm pitch at 60 rpm - an ordinary thread - plans to stop inside a
  // fifth of a single pulse.
  const float ordinaryThreadFeedPps = 0.25f * stepsPerMm;
  EXPECT_LT(reach(ordinaryThreadFeedPps), 1.0f);
}

// THE CENTRAL RESULT. Over the realistic sweep, a pass feeding away from a stop
// never once trips that stop's predicate - and the proof that the reconstruction
// above is not simply mis-sampling is the companion test below, which shows the
// carriage trace is identical with the stop and without it.
//
// The rear stop is placed ONE PULSE behind the start rather than on it. That is
// deliberate and it is not a fudge: standing exactly on a stop makes
// hitLeftEndstop() true as well, which brings the endstop ARREST into the
// picture (and, at low feeds, the defect at the bottom of this file). One pulse
// clear isolates goingToHitLeftEndstop as the only thing being measured, which
// is the whole point of this test. The both-stops case, carriage on the stop,
// is covered separately in ANormalThreadingSetupRunsThePassWithoutStalling.
TEST_F(MidPassStallTest, ARealisticPassNeverTripsTheStopBehindIt) {
  for (float pitch : {0.25f, 1.0f, 3.0f}) {
    for (int pps : kSpeeds) {
      const int spindlePulses = (int)(2000 / ratioFor(pitch));
      const PassRun r = pass(pitch, pps, -1, 4000, spindlePulses);

      EXPECT_EQ(r.rearTrips, 0)
          << "pitch " << pitch << " at " << pps << " PPS: the stop behind the "
          << "carriage was judged reachable on " << r.rearTrips << " of "
          << r.pulses << " pulses, furthest out at " << r.maxTripDistance
          << " pulses. An axis that accelerated from rest at that stop cannot "
          << "need more room to halt than it has already travelled.";
      EXPECT_EQ(r.mode, (int)GlobalMotionMode::MM_ENABLED)
          << "pitch " << pitch << " at " << pps << " PPS: the pass ended in "
          << "mode " << r.mode << " - something arrested it";
      EXPECT_GT(r.finalPos, 1900)
          << "pitch " << pitch << " at " << pps << " PPS: the pass did not run";
    }
  }
}

// The A/B, and the strongest evidence in the file: the same pass, once with the
// rear stop set and once with it not set at all, produces the SAME carriage
// position on every single iteration. Whatever the rear predicate is doing, it
// is not moving the carriage - so no fix to it can change a realistic pass, and
// no realistic pass can be blamed on it.
TEST_F(MidPassStallTest, SettingTheStopBehindTheCarriageChangesNothingAboutThePass) {
  for (float pitch : {0.25f, 1.0f, 3.0f}) {
    for (int pps : kSpeeds) {
      const int spindlePulses = (int)(2000 / ratioFor(pitch));
      const PassRun withStop = pass(pitch, pps, -1, 4000, spindlePulses, true);
      const PassRun without = pass(pitch, pps, INT32_MIN, 4000, spindlePulses, true);

      ASSERT_EQ(withStop.trace.size(), without.trace.size());
      size_t diverged = withStop.trace.size();
      for (size_t i = 0; i < withStop.trace.size(); ++i) {
        if (withStop.trace[i] != without.trace[i]) {
          diverged = i;
          break;
        }
      }
      EXPECT_EQ(diverged, withStop.trace.size())
          << "pitch " << pitch << " at " << pps << " PPS: the trace diverged at "
          << "iteration " << diverged << " (" << withStop.trace[diverged]
          << " with the rear stop set, " << without.trace[diverged]
          << " without) - the rear stop DID alter the pass";
      EXPECT_FLOAT_EQ(withStop.maxErr, without.maxErr)
          << "pitch " << pitch << " at " << pps
          << " PPS: the rear stop cost tracking accuracy";
    }
  }
}

// The normal threading setup, and the one the brief asks for by name: both
// stops set, the pass starts ON one of them and feeds towards the other. This
// is the case where the rear predicate is unavoidably true on the first pulse
// (standing on a stop, any stopping distance at all reaches it), so if it could
// do harm anywhere it would be here.
//
// It does not. The pass runs, the carriage travels, the mode stays MM_ENABLED
// with the axis actually moving, and it never goes stationary for longer than
// the ordinary gap between two steps.
//
// 1200 and 3000 PPS only. At 300 PPS with a 0.25 mm pitch this setup fails for
// an unrelated reason - see the last section - and asserting it here would
// blame the wrong mechanism.
TEST_F(MidPassStallTest, ANormalThreadingSetupRunsThePassWithoutStalling) {
  for (float pitch : {0.25f, 1.0f, 3.0f}) {
    for (int pps : {1200, 3000}) {
      buildRig();
      ls->setTargetPitchMM(pitch);
      ls->setCurrentPosition(0);
      ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);   // anchor + rear
      ls->setStopPosition(LeadscrewStopPosition::RIGHT, 4000);
      ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
      gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

      const int spindlePulses = (int)(2000 / ratioFor(pitch));
      const double perStep = (double)pps * (double)kDt / 1e6;
      double carry = 0.0;
      int delivered = 0, lastPos = 0, still = 0, longestStill = 0;
      while (delivered < spindlePulses) {
        advanceMockMicros(kDt);
        carry += perStep;
        int whole = (int)carry;
        if (whole > 0) {
          if (delivered + whole > spindlePulses) whole = spindlePulses - delivered;
          carry -= (double)((int)carry);
          spindle->incrementCurrentPosition(whole);
          delivered += whole;
        }
        ls->update();
        const int pos = ls->getCurrentPosition();
        if (pos == lastPos) {
          if (++still > longestStill) longestStill = still;
        } else {
          still = 0;
          lastPos = pos;
        }
      }

      EXPECT_GT(ls->getCurrentPosition(), 1900)
          << "pitch " << pitch << " at " << pps
          << " PPS: the pass did not reach the far end";
      EXPECT_EQ((int)gs->getMotionMode(), (int)GlobalMotionMode::MM_ENABLED)
          << "pitch " << pitch << " at " << pps << " PPS: the pass was arrested";
      // The slowest point of this sweep steps about every 130 iterations of a
      // 100 us loop; anything an order of magnitude past that is the carriage
      // sitting still with the mode still saying CUTTING, which is the
      // signature being hunted.
      EXPECT_LT(longestStill, 2000)
          << "pitch " << pitch << " at " << pps << " PPS: the carriage was "
          << "stationary for " << longestStill << " consecutive iterations "
          << "while the mode still read MM_ENABLED";
    }
  }
}

// CHARACTERIZATION, past the end of the realistic sweep. At 600 rpm - four
// times the top of the sweep, and not a speed anyone single-point threads at -
// the rear predicate does finally register, because the pass is running fast
// enough that one acceleration step of float rounding straddles the boundary.
//
// Recorded rather than asserted-as-a-defect because the effect is a single
// pulse: one trip per pass, 46 pulses (0.15 mm) off the stop, and three pulses
// of travel out of seven hundred. It is here so that "the predicate literally
// never fires" is not over-claimed, and so the boundary behaviour is on record
// if the acceleration constants ever change.
TEST_F(MidPassStallTest, PastTheRealisticSweepTheBoundaryIsGrazedNotCrossed) {
  const int pps = 12000;  // 600 rpm
  const float pitch = 3.0f;
  const int spindlePulses = (int)(2000 / ratioFor(pitch));

  const PassRun onStop = pass(pitch, pps, 0, 1000000, spindlePulses);
  const PassRun clear = pass(pitch, pps, -1, 1000000, spindlePulses);

  EXPECT_LE(onStop.rearTrips, 2)
      << "grazing the boundary is one thing; " << onStop.rearTrips
      << " trips would be the predicate genuinely governing the pass";
  EXPECT_EQ(clear.rearTrips, 0)
      << "one pulse clear of the stop, even 600 rpm does not reach it";
  // The cost of those trips, in travel, against the arm that never tripped.
  EXPECT_NEAR(onStop.finalPos, clear.finalPos, 10)
      << "the graze cost real travel: " << clear.finalPos - onStop.finalPos
      << " pulses";
  EXPECT_EQ(onStop.mode, (int)GlobalMotionMode::MM_ENABLED);
}

// ===========================================================================
// PART 3 - severity, if the predicate IS made to fire on a moving axis.
//
// Part 2 says a real pass cannot get there. This part answers the question that
// still matters if some other path ever does - a speed that is not real motion
// (the stale-ramp bug CLAUDE.md records under the endstop arrest), an encoder
// glitch saturating posError into a sprint, a future feature that moves a stop.
// The distinction the brief asks for is between a genuine stall and a transient,
// and the answer is unambiguous.
// ===========================================================================

// CHARACTERIZATION of the forced case, and a hard assertion that it is not a
// stall.
//
// The setup is synthetic and says so: the axis is run up to speed with no rear
// stop, and only then is a stop planted 10 pulses behind it, which the panel
// cannot do (UiState refuses every stop edit while the carriage is under power).
// It is the minimum manipulation that makes goingToHitLeftEndstop true on a
// carriage travelling away, which is precisely the hypothesis.
//
// What happens, measured, against a matched control that plants no stop:
//
//   pitch  spindle    commanded v   worst |error|      worst |error|
//                     before plant  with the stop      control
//   1.0 mm  3000 PPS      659 pps        2.3               2.3
//   1.0 mm 12000 PPS     3326 pps       30.4               1.4
//   3.0 mm  3000 PPS     2450 pps       13.8               1.9
//   3.0 mm 12000 PPS    12598 pps      981.8             636.6
//
// So it is real: the axis decelerates (commanded speed dipped 3326 -> 2445 pps
// in the second row) and the tool falls behind the helix by up to a third of a
// millimetre in the cases that are anywhere near reachable. But it recovers on
// its own every time, because the two sides of the comparison move in opposite
// directions - the carriage keeps stepping, so the distance to the stop grows,
// while the commanded speed falls, and the stopping distance falls as its
// SQUARE. The predicate had gone quiet by iteration ~1050 of 20000 even in the
// worst row.
//
// The three things that would make it the field report, and none of them
// happen: the carriage never stops (its longest stationary run is the same as
// the control's), the total travel over the run is identical to the control's,
// and the mode never sits at MM_ENABLED with the axis dead.
TEST_F(MidPassStallTest, PlantedRearStopDeceleratesButDoesNotStall) {
  const float pitch = 3.0f;
  const int pps = 12000;

  int advanced[2] = {0, 0};
  int longestStill[2] = {0, 0};
  float worstErr[2] = {0, 0};

  for (int arm = 0; arm < 2; ++arm) {  // 0 = plant a rear stop, 1 = control
    buildRig();
    ls->setTargetPitchMM(pitch);
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 1000000);
    ls->setSyncPoint();
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    const double perStep = (double)pps * (double)kDt / 1e6;
    double carry = 0.0;
    for (int i = 0; i < 20000; ++i) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        carry -= (double)whole;
        spindle->incrementCurrentPosition(whole);
      }
      ls->update();
    }

    const int startPos = ls->getCurrentPosition();
    if (arm == 0) {
      ASSERT_GT(ls->getLeadscrewSpeedPulsesPerSecond(), 5000.0f)
          << "the axis must actually be at speed for this to test anything";
      ls->setStopPosition(LeadscrewStopPosition::LEFT, startPos - 10);
    }

    int lastPos = startPos, still = 0;
    for (int i = 0; i < 20000; ++i) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        carry -= (double)whole;
        spindle->incrementCurrentPosition(whole);
      }
      ls->update();
      const int pos = ls->getCurrentPosition();
      const float e = fabsf(ls->getPositionError());
      if (e > worstErr[arm]) worstErr[arm] = e;
      if (pos == lastPos) {
        if (++still > longestStill[arm]) longestStill[arm] = still;
      } else {
        still = 0;
        lastPos = pos;
      }
    }
    advanced[arm] = ls->getCurrentPosition() - startPos;
    EXPECT_EQ((int)gs->getMotionMode(), (int)GlobalMotionMode::MM_ENABLED);
  }

  // NOT A STALL - the properties the field report would have violated.
  EXPECT_EQ(advanced[0], advanced[1])
      << "the planted rear stop cost the pass " << advanced[1] - advanced[0]
      << " pulses of travel over 20000 iterations - it did not just delay the "
         "axis, it held it back permanently";
  EXPECT_LE(longestStill[0], longestStill[1] + 2)
      << "the carriage went stationary for " << longestStill[0]
      << " iterations against the control's " << longestStill[1];

  // ...but not free either. Recorded, not required: this is the real cost of
  // the direction-agnostic predicate, and it is the number to look at if a fix
  // is ever weighed up.
  EXPECT_GT(worstErr[0], worstErr[1])
      << "CHARACTERIZATION, not a requirement: the planted rear stop is "
         "expected to cost tracking accuracy (measured 981.8 pulses of "
         "following error against the control's 636.6). If this ever fails it "
         "means the rear predicate no longer decelerates a receding axis - i.e. "
         "somebody made it direction-aware. That is a fix, not a regression: "
         "delete this expectation rather than restoring the behaviour.";
}

// The mirror: a left-hand thread feeding LEFT, with the RIGHT stop planted
// behind it. Same conclusion, and it is worth having explicitly because the two
// predicates are separate expressions and a fix could easily correct one.
TEST_F(MidPassStallTest, PlantedRearStopMirrorForALeftHandThread) {
  buildRig();
  ls->setTargetPitchMM(-3.0f);  // left-hand thread: positive spindle feeds LEFT
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -1000000);
  ls->setSyncPoint();
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  const double perStep = 12000.0 * (double)kDt / 1e6;
  double carry = 0.0;
  for (int i = 0; i < 20000; ++i) {
    advanceMockMicros(kDt);
    carry += perStep;
    int whole = (int)carry;
    if (whole > 0) { carry -= (double)whole; spindle->incrementCurrentPosition(whole); }
    ls->update();
  }
  const int startPos = ls->getCurrentPosition();
  ASSERT_LT(startPos, -1000) << "the carriage should be well left of centre";
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, startPos + 10);

  int lastPos = startPos, still = 0, longestStill = 0;
  for (int i = 0; i < 20000; ++i) {
    advanceMockMicros(kDt);
    carry += perStep;
    int whole = (int)carry;
    if (whole > 0) { carry -= (double)whole; spindle->incrementCurrentPosition(whole); }
    ls->update();
    const int pos = ls->getCurrentPosition();
    if (pos == lastPos) {
      if (++still > longestStill) longestStill = still;
    } else {
      still = 0;
      lastPos = pos;
    }
  }

  EXPECT_LT(ls->getCurrentPosition(), startPos - 1000)
      << "the carriage must keep feeding left away from the planted stop";
  EXPECT_EQ((int)gs->getMotionMode(), (int)GlobalMotionMode::MM_ENABLED);
  EXPECT_LT(longestStill, 50)
      << "stationary for " << longestStill << " iterations - a stall, not a dip";
}

// ===========================================================================
// PART 4 - what actually turned up while the hypothesis was being refuted.
//
// NOT the rear predicate, and NOT the field report either - but a real
// phenomenon found by the sweep above, recorded here rather than dropped.
//
// THE OBSERVATION. Both stops set, the carriage on the left one, a 0.25 mm
// right-hand thread, spindle at 300 PPS (15 rpm - a wholly ordinary
// single-point threading speed, arguably the ordinary one). The pass never
// starts. Step by step, from the instrumented run:
//
//   i=533  pos=1  dir=RIGHT  err=+0.05   first step off the stop
//   i=567  pos=2  dir=RIGHT  err=-0.89   overshot: the axis cannot step slower
//   i=631  pos=3  dir=RIGHT  err=-1.82   overshot again
//   i=632  pos=3  dir=LEFT              err < -1, so the direction latch flips
//   i=793  pos=0  dir=LEFT   err=+1.51   walked all the way back onto the stop
//   i=794         mode=MM_DISABLED       hitLeftEndstop && dir==LEFT -> arrest
//
// THE MECHANISM is the minimum step rate, not the stops. m_currentPulseDelay is
// clamped to initialPulseDelay, which is LEADSCREW_JERK (0.5 mm/s) worth of
// steps. A 0.25 mm thread at 15 rpm demands 19.7 pps, well under that floor.
// The axis physically cannot feed that slowly, so it steps, overshoots, drives
// the following error past -1, flips the direction latch and steps back - and
// because the pass began standing on the stop, the first backward step lands
// on it and the arrest fires.
//
// (An earlier note here computed the floor as "157 pps, halved to ~79 by
// sendPulse()'s two-call toggle". That halving does not happen: board.h
// defines ELS_USE_RMT unconditionally, so sendPulse() always takes the RMT
// branch and always returns true - the pin-toggle branch the halving
// describes is dead code, on the host build and the real device alike. The
// floor is the un-halved ~157 pps. Verified directly in
// test/test_midfeed_tracking by measuring the interval between real steps: it
// is capped at initialPulseDelay and never doubles it. This does not change
// any conclusion below, since every number here came from observing pass/fail
// outcomes rather than from this arithmetic.)
//
// Measured threshold, sweeping the demanded feed: the pass fails below about
// 60 leadscrew pps (~0.19 mm/s at the carriage) and runs above about 66. That
// is 0.25 mm pitch below 1000 PPS (50 rpm), 0.5 mm below 500, 1.0 mm below 300 -
// i.e. it gets WORSE the finer the thread and the slower the spindle, which is
// the direction a nervous operator moves in. The left-hand mirror off the right
// stop behaves identically.
//
// WHY IT IS NOT THE FIELD REPORT, despite being a "the cut stopped" bug: it
// resolves to MM_DISABLED, so the panel reads IDLE and the keypad is live. The
// report was MM_ENABLED - CUTTING - with the panel locked out. It also happens
// at the START of a pass, not mid-pass.
//
// RE-SCOPED (issue #1). This section originally asserted the arrest above as a
// defect via AFineThreadAtLowRpmMustActuallyLeaveItsStop, on the premise that a
// pass starts parked on a stop. Issue #8 has since ruled that premise false: a
// pass ends at its stop and is finished there, so a fresh pass never starts
// standing on one - the operator withdraws, returns and re-engages away from
// it. That test asserted the wrong requirement and has been REMOVED.
//
// The mechanism above is still real (the axis genuinely cannot step slower
// than the initialPulseDelay floor) and it is still worth recording here,
// because THIS scenario - starting a pass standing on a stop - is exactly what
// trips it: the very first overshoot-and-reverse of the dither lands on the
// stop the carriage never left, and the endstop arrest (correctly) fires. The
// question of whether the underlying dither is a problem AWAY from a stop -
// which is what issue #1 actually needed answered - is measured directly in
// test/test_midfeed_tracking, mid-pass, with no stop nearby to trip: the
// answer there is no. The dither is bounded (worst observed following error
// 4.35 pulses / 0.0138 mm) and does not cost pitch accuracy over dozens of
// revolutions; it is a surface-finish question at worst, not a stall or a
// pitch defect. This suite's TheLowFeedDitherThresholdIsAboutSixtyPulsesPerSecond
// below is kept AS IS - it still correctly characterizes when THIS scenario
// (starting on a stop) arrests - but it is deliberately not read as a general
// tracking limit any more.
//
// SCOPE NOTE for whoever picks this up: the arrest that fires here is the same
// one test/test_endstop_deadlock is about, so a fix probably belongs with that
// work rather than here - but this is a DIFFERENT trigger (a fresh pass at low
// feed, not a re-engage onto a stop the carriage was parked on) and it is not
// covered by any test in that suite. The cure is more likely to be in the
// direction latch's -1 threshold, or in refusing to step at all when the
// demanded feed is below the minimum step rate, than in the arrest itself -
// bearing in mind that test_midfeed_tracking now fences the mid-pass case, so
// any such fix must not change LowFeedDitherStaysBoundedAndPitchIsExact,
// LongRunFollowingErrorDoesNotAccumulate or the ordinary-speed regression
// fence in that suite.
// ===========================================================================

// REMOVED: AFineThreadAtLowRpmMustActuallyLeaveItsStop asserted that this
// exact setup (both stops set, carriage starting ON the left one) must reach
// >100 pulses of travel within four spindle revolutions. Its premise - that a
// fresh pass starts parked on a stop - is false per issue #8: a pass ends at
// its stop and is finished there, so this exact starting condition does not
// arise on the machine. See the PART 4 header comment above for the full
// re-scoping and where the real question (is the underlying dither a problem
// AWAY from a stop) is now answered: test/test_midfeed_tracking, answer no.

// CHARACTERIZATION of the boundary of the arrest above, so a fix aimed at it
// can tell the threshold moved, and so a regression is visible as a shift in
// the threshold rather than as one test flipping. Recorded, not judged: these
// are the observed pass/fail feeds for THIS scenario specifically - carriage
// starting on the stop it is about to feed away from. It says nothing about
// tracking quality away from a stop; that question belongs to
// test/test_midfeed_tracking, not here.
TEST_F(MidPassStallTest, TheLowFeedDitherThresholdIsAboutSixtyPulsesPerSecond) {
  struct Point { float pitch; int pps; bool expectStarts; };
  const Point points[] = {
      // demanded carriage feed, leadscrew pulses/sec, in the comment
      {0.25f,   300, false},  //  19.7 pps
      {0.25f,   800, false},  //  52.5 pps
      {0.25f,  1000, true},   //  65.6 pps
      {0.25f,  1200, true},   //  78.7 pps
      {0.50f,   400, false},  //  52.5 pps
      {0.50f,   500, true},   //  65.6 pps
      {1.00f,   200, false},  //  52.5 pps
      {1.00f,   300, true},   //  78.7 pps
  };

  for (const Point& p : points) {
    buildRig();
    ls->setTargetPitchMM(p.pitch);
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 4000);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    const int spindlePulses = (int)(1000 / ratioFor(p.pitch));
    const double perStep = (double)p.pps * (double)kDt / 1e6;
    double carry = 0.0;
    int delivered = 0;
    while (delivered < spindlePulses) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        if (delivered + whole > spindlePulses) whole = spindlePulses - delivered;
        carry -= (double)((int)carry);
        spindle->incrementCurrentPosition(whole);
        delivered += whole;
      }
      ls->update();
      if (gs->getMotionMode() == MM_DISABLED) break;
    }

    const bool started = ls->getCurrentPosition() > 10;
    EXPECT_EQ(started, p.expectStarts)
        << "pitch " << p.pitch << " at " << p.pps << " PPS ("
        << (float)p.pps / encoderPPR() * p.pitch * derived->leadscrewStepsPerMm()
        << " leadscrew pps demanded): the pass "
        << (started ? "started" : "did not start")
        << ", carriage at " << ls->getCurrentPosition();
  }
}

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
