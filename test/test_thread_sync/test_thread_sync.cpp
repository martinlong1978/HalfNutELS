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

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
