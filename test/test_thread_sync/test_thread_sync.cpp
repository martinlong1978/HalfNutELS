// Thread-sync / helix-consistency tests for Leadscrew on the host.
//
// These prove the properties that matter for single-point threading:
//   * the leadscrew threads onto a CONSISTENT helix regardless of spindle
//     speed (the tool lands on the same spindle phase at the sync stop, every
//     pass, at every speed), and
//   * deceleration parks the tool ON the endstop.
// Both are checked for RIGHT-HAND threads (positive pitch) and LEFT-HAND /
// reverse threads (negative pitch, as FM_THREAD_REVERSE feeds via a negated
// ratio in Leadscrew::setTargetPitchMM).
//
// Realism
// -------
// The spindle is turned at a constant rate: each simulation step advances the
// virtual clock by a fixed dt (100 us) and delivers integer encoder pulses
// (carrying the fractional remainder), then calls ls->update(). TestSpindle now
// reports a real, time-based velocity (mirrors ESPSpindle), so the leadscrew's
// speed-dependent feed-forward is genuinely exercised. Speeds are swept across a
// realistic range for the default config (encoderPPR = 1200 -> 300/1200/3000
// PPS ~= 15/60/150 rpm).
//
#include <gmock/gmock.h>

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <iterator>
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
// Realistic spindle speeds in encoder pulses/sec (PPR = 1200 -> 15/60/150 rpm).
const int kSpeeds[] = {300, 1200, 3000};

class ThreadSyncTest : public ::testing::Test {
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
    buildRig();  // creates spindle + ls in a known state
  }

  void TearDown() override {
    delete ls;
    ls = nullptr;
    delete spindle;
    delete derived;
  }

  int encoderPPR() const { return derived->spindleEncoderPpr(); }

  // (Re)build the spindle + leadscrew in a fully known baseline state. Called
  // per speed so each sweep point is isolated (spindle velocity history too).
  void buildRig() {
    delete ls;
    ls = nullptr;
    delete spindle;
    spindle = nullptr;
    resetMockClock();
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    spindle = new Spindle(0, 1, derived);

    ls = new Leadscrew(derived, spindle, &io,
                       derived->accellerationPulseSec(),
                       derived->leadscrewInitialPulseDelay(),
                       derived->stepperPpr() * derived->gearboxRatio(),
                       derived->leadscrewPitchMm(), encoderPPR());
  }

  // Compute the leadscrew:spindle ratio for a pitch, matching setTargetPitchMM.
  float ratioFor(float pitch) const {
    return (pitch * (float)(derived->stepperPpr() * derived->gearboxRatio())) /
           (derived->leadscrewPitchMm() * (float)encoderPPR());
  }

  // Drive the spindle by `pulses` (signed) NET encoder counts at |pps| rate,
  // one kDt step at a time, calling ls->update() each step. Fractional pulses
  // are carried between steps so the average rate is exactly |pps|.
  void driveSpindle(int pulses, int pps) {
    const int dir = pulses >= 0 ? 1 : -1;
    const int target = pulses >= 0 ? pulses : -pulses;
    const double perStep = (double)pps * (double)kDt / 1e6;  // magnitude
    double carry = 0.0;
    int delivered = 0;
    while (delivered < target) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        if (delivered + whole > target) whole = target - delivered;
        carry -= (double)((int)carry);  // keep the fractional remainder
        spindle->incrementCurrentPosition(dir * whole);
        delivered += whole;
      }
      ls->update();
    }
  }

  // As driveSpindle, but samples the following error (and the carriage
  // position) after every update() and reports the worst excursion seen. The
  // final error alone is not enough: a leak that is later mopped up by the
  // re-sync gate still commands real motion while it lasts.
  void driveSpindleTracking(int pulses, int pps, float *maxAbsError,
                            int *maxAbsDriftFromStart) {
    const int dir = pulses >= 0 ? 1 : -1;
    const int target = pulses >= 0 ? pulses : -pulses;
    const double perStep = (double)pps * (double)kDt / 1e6;
    const int startPos = ls->getCurrentPosition();
    double carry = 0.0;
    int delivered = 0;
    *maxAbsError = 0.0f;
    *maxAbsDriftFromStart = 0;
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
      float e = fabsf(ls->getPositionError());
      if (e > *maxAbsError) *maxAbsError = e;
      int d = abs(ls->getCurrentPosition() - startPos);
      if (d > *maxAbsDriftFromStart) *maxAbsDriftFromStart = d;
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

  // Establish thread sync at a stop placed at the current carriage position
  // (which is 0, where expected == current so there is no spurious error).
  // Returns the captured spindleSyncPos (== spindle phase at this instant).
  int establishSyncAt(LeadscrewStopPosition stop) {
    ls->setCurrentPosition(0);
    ls->setStopPosition(stop);  // at pos 0 -> SS_SYNC, records spindleSyncPos
    EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC)
        << "sync was not established (constructor left sync state stale?)";
    return spindle->getCurrentPosition();
  }
};

// ===========================================================================
// RIGHT-HAND thread (positive pitch)
// ===========================================================================

// Property 1: the helix phase at the sync-stop landing is speed-independent and
// equals spindleSyncPos. Sync is anchored at the LEFT stop; we thread OUT to
// the right, re-arm the left stop, then thread back INTO it. At the settled
// landing the spindle phase must equal spindleSyncPos, for every speed.
TEST_F(ThreadSyncTest, RH_HelixPhaseEqualsSyncPosAndSpeedIndependent) {
  const float pitch = 0.25f;
  const int K = 2400;  // round-trip amplitude in spindle pulses (2 revs)

  std::vector<int> phases, syncPhases, landings;
  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(pitch);

    // Put the spindle at a non-trivial phase so the assertion is meaningful.
    spindle->incrementCurrentPosition(500);
    spindle->consumePosition();

    const int syncPhase = establishSyncAt(LeadscrewStopPosition::LEFT);
    // Disarm the left stop so we can thread OUT without instantly disabling
    // (the left endstop disables the axis on contact in MM_ENABLED).
    ls->unsetStopPosition(LeadscrewStopPosition::LEFT);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(+K, pps);  // positive pitch: carriage threads RIGHT
    settle();

    ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);  // re-arm at sync stop
    driveSpindle(-K, pps);  // thread back LEFT into the left stop
    settle();

    phases.push_back(positiveModulo(spindle->getCurrentPosition(),
                                    encoderPPR()));
    syncPhases.push_back(positiveModulo(syncPhase, encoderPPR()));
    landings.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 0; i < phases.size(); ++i) {
    // At the sync phase the tool is back on the sync (left) stop: this ties the
    // spindle phase to the carriage position, i.e. the same helix.
    EXPECT_NEAR(landings[i], 0, 2)
        << "tool not back on the sync stop at speed " << kSpeeds[i];
    EXPECT_EQ(phases[i], syncPhases[i])
        << "spindle phase != spindleSyncPos at speed " << kSpeeds[i];
  }
  for (size_t i = 1; i < phases.size(); ++i) {
    EXPECT_EQ(phases[i], phases[0])
        << "landing phase differs across speeds (" << kSpeeds[i] << " vs "
        << kSpeeds[0] << ")";
  }
}

// Property 2: pitch is correct - carriage travel vs spindle revolutions matches
// the configured ratio (leadscrew pulses per spindle rev ~= ratio*encoderPPR),
// independent of speed.
TEST_F(ThreadSyncTest, RH_PitchMatchesConfiguredRatio) {
  const float pitch = 0.25f;
  const float ratio = ratioFor(pitch);
  const int N = 6000;  // spindle pulses (5 revs)
  const float expectedTravel = N * ratio;

  std::vector<int> travels;
  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(pitch);
    establishSyncAt(LeadscrewStopPosition::LEFT);
    ls->unsetStopPosition(LeadscrewStopPosition::LEFT);  // free travel
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    driveSpindle(+N, pps);
    settle();
    travels.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 0; i < travels.size(); ++i) {
    // Per-pulse ratio should match to well under 1%.
    EXPECT_NEAR((double)travels[i] / N, ratio, ratio * 0.01)
        << "observed pitch wrong at speed " << kSpeeds[i];
    EXPECT_NEAR((float)travels[i], expectedTravel, 2.0f)
        << "carriage travel != N*ratio at speed " << kSpeeds[i];
  }
  for (size_t i = 1; i < travels.size(); ++i) {
    EXPECT_NEAR(travels[i], travels[0], 1)
        << "travel differs across speeds";
  }
}

// Property 3: deceleration parks the tool ON the left endstop (not past it).
// The spindle keeps turning steadily into the stop; the axis must decelerate
// and come to rest at the stop position.
TEST_F(ThreadSyncTest, RH_DecelerationLandsOnLeftEndstop) {
  const float pitch = 0.25f;
  const int leftStop = 0;

  std::vector<int> landings;
  for (size_t i = 0; i < std::size(kSpeeds); ++i) {
    const int pps = kSpeeds[i];
    buildRig();
    ls->setTargetPitchMM(pitch);
    establishSyncAt(LeadscrewStopPosition::LEFT);
    ls->unsetStopPosition(LeadscrewStopPosition::LEFT);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    // Thread well out to the right, then re-arm the left stop and thread back
    // into it, driving the spindle a full extra rev PAST the landing point so
    // the endstop has to arrest a still-turning thread.
    const int K = 3000;
    driveSpindle(+K, pps);
    settle();
    ls->setStopPosition(LeadscrewStopPosition::LEFT, leftStop);
    driveSpindle(-(K + 1200), pps);  // push a further rev into the stop
    settle();

    landings.push_back(ls->getCurrentPosition());
    // Key property: the axis ARRESTS on the left endstop (it disables itself and
    // does not keep running with the spindle).
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
        << "left endstop did not arrest the thread at speed " << pps;
  }

  for (size_t i = 0; i < landings.size(); ++i) {
    // The tool parks ON the left stop. Discrete deceleration can leave it a
    // single step past the stop (observed: exactly 0 at 300/3000 PPS, -1 at
    // 1200 PPS); it never runs away. See report for the <=1-pulse detail.
    EXPECT_LE(landings[i], leftStop)
        << "rested to the RIGHT of the left stop at speed " << kSpeeds[i];
    EXPECT_GE(landings[i], leftStop - 1)
        << "overran the left stop by more than one step at speed "
        << kSpeeds[i] << " (got " << landings[i] << ")";
  }
}

// ===========================================================================
// LEFT-HAND / reverse thread (negative pitch)
// ===========================================================================

// Property 4: mirror of 1. Sync anchored at the RIGHT stop; thread OUT to the
// left, re-arm the right stop, thread back into it. Phase lands on
// spindleSyncPos at the right stop, for every speed.
TEST_F(ThreadSyncTest, LH_HelixPhaseEqualsSyncPosAndSpeedIndependent) {
  const float pitch = -0.25f;
  const int K = 2400;

  std::vector<int> phases, syncPhases, landings;
  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(pitch);

    spindle->incrementCurrentPosition(500);
    spindle->consumePosition();

    const int syncPhase = establishSyncAt(LeadscrewStopPosition::RIGHT);
    ls->unsetStopPosition(LeadscrewStopPosition::RIGHT);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(+K, pps);  // negative pitch: carriage threads LEFT
    settle();

    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);  // re-arm sync stop
    driveSpindle(-K, pps);  // thread back RIGHT into the right stop
    settle();

    phases.push_back(positiveModulo(spindle->getCurrentPosition(),
                                    encoderPPR()));
    syncPhases.push_back(positiveModulo(syncPhase, encoderPPR()));
    landings.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 0; i < phases.size(); ++i) {
    // At the sync phase the tool is back on the sync (right) stop.
    EXPECT_NEAR(landings[i], 0, 2)
        << "tool not back on the sync stop at speed " << kSpeeds[i];
    EXPECT_EQ(phases[i], syncPhases[i])
        << "spindle phase != spindleSyncPos at speed " << kSpeeds[i];
  }
  for (size_t i = 1; i < phases.size(); ++i) {
    EXPECT_EQ(phases[i], phases[0])
        << "landing phase differs across speeds";
  }
}

// Property 5: reverse-thread pitch magnitude matches the ratio, and travel is
// in the opposite (left) direction, independent of speed.
TEST_F(ThreadSyncTest, LH_PitchMatchesConfiguredRatio) {
  const float pitch = -0.25f;
  const float ratio = ratioFor(pitch);  // negative
  const int N = 6000;
  const float expectedTravel = N * ratio;

  std::vector<int> travels;
  for (int pps : kSpeeds) {
    buildRig();
    ls->setTargetPitchMM(pitch);
    establishSyncAt(LeadscrewStopPosition::RIGHT);
    ls->unsetStopPosition(LeadscrewStopPosition::RIGHT);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    driveSpindle(+N, pps);
    settle();
    travels.push_back(ls->getCurrentPosition());
  }

  for (size_t i = 0; i < travels.size(); ++i) {
    EXPECT_LT(travels[i], 0) << "reverse thread should travel left";
    EXPECT_NEAR((double)travels[i] / N, ratio, (-ratio) * 0.01)
        << "observed reverse pitch wrong at speed " << kSpeeds[i];
    EXPECT_NEAR((float)travels[i], expectedTravel, 2.0f)
        << "carriage travel != N*ratio at speed " << kSpeeds[i];
  }
  for (size_t i = 1; i < travels.size(); ++i) {
    EXPECT_NEAR(travels[i], travels[0], 1) << "travel differs across speeds";
  }
}

// Property 6: deceleration parks the tool ON the right endstop (not past it),
// for a reverse thread. (This originally failed and surfaced a real bug: the
// "hit an endstop -> disable" logic in Leadscrew::update() only arrested on the
// LEFT endstop in MM_ENABLED, so a reverse thread was dragged straight past the
// RIGHT stop. Fixed by making the disable direction-aware.)
TEST_F(ThreadSyncTest, LH_DecelerationLandsOnRightEndstop) {
  const float pitch = -0.25f;
  const int rightStop = 0;

  std::vector<int> landings;
  for (size_t i = 0; i < std::size(kSpeeds); ++i) {
    const int pps = kSpeeds[i];
    buildRig();
    ls->setTargetPitchMM(pitch);
    establishSyncAt(LeadscrewStopPosition::RIGHT);
    ls->unsetStopPosition(LeadscrewStopPosition::RIGHT);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

    const int K = 3000;
    driveSpindle(+K, pps);  // carriage well to the left
    settle();
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, rightStop);
    driveSpindle(-(K + 1200), pps);  // push a further rev into the stop
    settle();

    landings.push_back(ls->getCurrentPosition());
    // CORRECT property: the right endstop should arrest the reverse thread.
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
        << "right endstop did not arrest the reverse thread at speed " << pps
        << " (KNOWN BUG: only the left endstop disables in MM_ENABLED)";
  }

  for (size_t i = 0; i < landings.size(); ++i) {
    // The tool parks ON the right stop; discrete deceleration can leave it a
    // single step past in the direction of travel (+1), mirroring the RH case
    // (which lands at -1). It never runs away.
    EXPECT_GE(landings[i], rightStop)
        << "did not reach the right stop at speed " << kSpeeds[i] << " (got "
        << landings[i] << ")";
    EXPECT_LE(landings[i], rightStop + 1)
        << "overran the right stop by more than one step at speed "
        << kSpeeds[i] << " (got " << landings[i] << ")";
  }
}
// ===========================================================================
// Arrest at an endstop with the SPINDLE STILL TURNING
// ===========================================================================
//
// Properties 3 and 6 both stop the spindle (settle()) immediately after the
// arrest. On a real lathe the operator does not: the chuck keeps turning after
// the tool parks on the stop. That is the case these cover, and it is the one
// the UI's "is the carriage under power" logic depends on - UiContext derives
// that from the COMMANDED motion mode, which reads MM_DISABLED from the instant
// the endstop arrests.
//
// The state under test: a spindle-sync anchor is latched (setStopPosition() at
// the current carriage position, never unset - so syncArmed() stays true), the
// axis threads into that stop, and the arrest publishes MM_DISABLED + SS_UNSYNC
// together. From that iteration on:
//
//   * jogMode is false (MM_DISABLED has no MMF_JOG bit), so the non-jog branch
//     at the top of update() keeps folding consumed spindle pulses into
//     m_expectedPosition while m_currentPosition is parked on the stop;
//   * the re-sync gate's short-circuit return (syncArmed() && SS_UNSYNC &&
//     !jogMode) skips sendPulse(), which is the only place m_leadscrewSpeed
//     decays - so if the arrest left residual speed, the MM_DISABLED re-pin
//     (m_expectedPosition = m_currentPosition, guarded by m_leadscrewSpeed == 0)
//     does not fire either.
//
// Residual speed at the arrest IS reachable (the deceleration planner does not
// always land on exactly zero: measured 0 at 300 PPS but ~7 at 1200 and ~143 at
// 3000 for this 0.25 mm pitch), so the accumulation above really does happen.
// What bounds it is that the re-sync search runs BEFORE the short-circuit
// return: it re-fires within one spindle revolution, and its
//     m_expectedPosition = m_currentPosition - pulsesToTargetSpeed
// throws the accumulation away and re-opens the gate so the speed can decay to
// zero and the re-pin can latch.
//
// So the invariant these tests pin is "bounded, and NOT proportional to how
// long the spindle is left turning": the peak following error must stay inside
// roughly one revolution of feed, the second ten revolutions must not be worse
// than the first ten, and the carriage must not creep off the stop. A leak
// behind the gate would grow by a whole pitch per revolution and fail all three.
class ArrestedThreadTest : public ThreadSyncTest {
 protected:
  // Thread into the stop, stopping the instant the endstop arrest latches, so
  // the measurement window starts exactly at MM_DISABLED. Returns false if it
  // never arrested.
  bool driveUntilArrest(int pps, int maxSteps = 400000) {
    const double perStep = (double)pps * (double)kDt / 1e6;
    double carry = 0.0;
    for (int i = 0; i < maxSteps; ++i) {
      advanceMockMicros(kDt);
      carry += perStep;
      int whole = (int)carry;
      if (whole > 0) {
        carry -= (double)whole;
        spindle->incrementCurrentPosition(-whole);
      }
      ls->update();
      if (gs->getMotionMode() == GlobalMotionMode::MM_DISABLED) return true;
    }
    return false;
  }

  // One spindle revolution of carriage feed, in leadscrew pulses: the natural
  // unit for "how much error one revolution behind the gate is worth".
  float onePitchInPulses(float pitch) const {
    return fabsf(ratioFor(pitch)) * (float)encoderPPR();
  }

  void runArrestedSpindleKeepsTurning(float pitch, LeadscrewStopPosition stop,
                                      const char *tag) {
    const float onePitch = onePitchInPulses(pitch);
    const int revs = 10;

    for (size_t i = 0; i < std::size(kSpeeds); ++i) {
      const int pps = kSpeeds[i];
      buildRig();
      ls->setTargetPitchMM(pitch);
      // Anchor at the stop and LEAVE IT SET - the ordinary threading setup (a
      // stop at the start of the cut), and what keeps syncArmed() true through
      // the arrest.
      establishSyncAt(stop);

      gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
      driveSpindle(+6000, pps);  // thread OUT, away from the sync stop
      settle();
      ASSERT_TRUE(driveUntilArrest(pps))
          << tag << ": endstop did not arrest the thread at speed " << pps;

      const float errorAtArrest = ls->getPositionError();
      const int posAtArrest = ls->getCurrentPosition();

      // The operator has NOT stopped the lathe. Two equal windows: a leak
      // behind the gate makes the second strictly worse than the first.
      float maxErr1 = 0.0f, maxErr2 = 0.0f;
      int drift1 = 0, drift2 = 0;
      driveSpindleTracking(-revs * encoderPPR(), pps, &maxErr1, &drift1);
      driveSpindleTracking(-revs * encoderPPR(), pps, &maxErr2, &drift2);

      // Bounded: within about one revolution of feed, not growing with time.
      EXPECT_LE(maxErr1, onePitch * 1.5f)
          << tag << ": following error accumulated behind the re-sync gate "
             "after the endstop arrest at speed "
          << pps << " (err at arrest " << errorAtArrest << ", peak " << maxErr1
          << " over " << revs << " spindle revolutions, one revolution of feed "
             "is " << onePitch << " pulses)";
      EXPECT_LE(maxErr2, maxErr1 + 1.0f)
          << tag << ": following error keeps GROWING with spindle rotation at "
             "speed "
          << pps << " (peak " << maxErr1 << " over revolutions 1-" << revs
          << ", " << maxErr2 << " over " << (revs + 1) << "-" << (2 * revs)
          << ") - this is the self-sustaining leak, not a transient";

      // And nothing moves: MM_DISABLED must mean the carriage stays put.
      EXPECT_LE(drift1, 2) << tag << ": carriage moved after the arrest at "
                           << pps;
      EXPECT_LE(drift2, 2) << tag << ": carriage moved after the arrest at "
                           << pps;
      EXPECT_LE(abs(ls->getCurrentPosition() - posAtArrest), 2)
          << tag << ": carriage crept off the stop at speed " << pps;
      EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
          << tag << ": axis re-enabled itself at speed " << pps;
    }
  }
};

TEST_F(ArrestedThreadTest, RH_SpindleKeptTurningAfterArrestDoesNotWindUpError) {
  runArrestedSpindleKeepsTurning(0.25f, LeadscrewStopPosition::LEFT, "RH");
}

// Mirror for a LEFT-HAND / reverse thread arresting on the RIGHT stop. Swept
// separately because the deceleration planner can land on exactly zero speed
// for one hand or speed and not another, which would make any leak
// intermittent rather than absent.
TEST_F(ArrestedThreadTest, LH_SpindleKeptTurningAfterArrestDoesNotWindUpError) {
  runArrestedSpindleKeepsTurning(-0.25f, LeadscrewStopPosition::RIGHT, "LH");
}

// Same invariant at a coarse pitch, where the arrest reliably leaves residual
// leadscrew speed (measured ~120-340 PPS at 1.5 mm) and so genuinely exercises
// the held-gate path rather than the "planner happened to land on zero" one.
// The peak error here is legitimately large - a whole revolution of a 1.5 mm
// feed is ~470 pulses - which is exactly why the "does not grow" half of the
// assertion, not the absolute bound, is the one that matters.
TEST_F(ArrestedThreadTest, CoarsePitchArrestErrorIsBoundedByOneRevolution) {
  runArrestedSpindleKeepsTurning(1.5f, LeadscrewStopPosition::LEFT, "RH-coarse");
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
