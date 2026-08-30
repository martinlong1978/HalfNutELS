// The endstop / re-engage deadlock in Leadscrew::update().
//
// NOT THE FIELD BUG. This suite was written while chasing a report from the
// lathe (Aug 2026, mid-film) of a thread cut stopping with the panel dead and
// the state word still reading CUTTING. The owner then established that the
// stall happened MID-PASS, nowhere near an endstop, which rules this out as the
// cause - and separately that the lathe was very likely running the EP2 demo
// branch, whose deliberately naive sync-start over-speeds at the start of a cut.
//
// It is kept because the hole is real, reachable from the panel, and produces
// that same signature. "CUTTING" is printed for MM_ENABLED and for nothing else
// (ST7789_320_240displaylvgl.cpp); ButtonPad publishes
// ctx.motionEnabled = (mode == MM_ENABLED), and UiState's underPower() gates the
// knob, the stop edits and the menu tiles on exactly that. So an axis stuck in
// MM_ENABLED with no steps left to give locks the panel out by itself, and a
// future report of "it says CUTTING and nothing works" should not have to
// rediscover that this state is possible.
//
// THE MECHANISM, in three parts of Leadscrew::update():
//
//   * the arrest (leadscrew.cpp, "If we've hit an endstop") only fires when the
//     DIRECTION LATCH agrees with the stop being touched:
//         MM_ENABLED && hitLeftEndstop && m_currentDirection == LEFT
//   * the stepping branches below it are gated on the STOP ALONE:
//         ... && !hitLeftEndstop
//   * and m_currentDirection is reset to UNKNOWN once the axis settles, in the
//     MM_DISABLED/MM_DECELLERATE branch between them.
//
// So the operator's own recovery arms it. Arrive at the left stop travelling
// LEFT: the arrest fires correctly, the axis settles, and the direction latch
// goes UNKNOWN. Now press ENABLE while still parked on that stop. The mode is
// MM_ENABLED again, the stop is still under the carriage, but the latch no
// longer says LEFT - so the arrest cannot fire, and the left stepping branch is
// blocked by !hitLeftEndstop. The spindle keeps turning, the following error
// grows without bound, and NOTHING resolves it: m_currentDirection is only ever
// assigned inside the two branches that can no longer run.
//
// WHAT THESE TESTS ASSERT. Per the repo's convention the stall tests assert the
// CORRECT behaviour and are expected to FAIL until the fix lands. The rest pin
// what the fix must not break, including CHARACTERIZATION tests recording
// behaviour that is currently unexplained-but-safe rather than asserting a guess
// about what it ought to be - see the notes on each.
//
// The file is in six blocks, in this order:
//
//   1. baseline - the arrest works, and the settled arrest releases the latch;
//   2. the deadlock, all four stop x hand-of-thread combinations, on the
//      minimal parkArrestedOn() rig (FAILING);
//   3. the same deadlock on the machine the operator actually has - both stops
//      set, helix anchor armed, a real out-and-back pass - plus the following-
//      error bound and the start-the-spindle-later ordering (FAILING);
//   4. what the fix must NOT break: the identical rig fed the OTHER way, which
//      must still run a whole pass. Read the long note on
//      AFinishedPassReEngagedAndFedAwayFromItsStopStillRuns before writing the
//      fix - it carries the one measurement that decides which signal the
//      arrest can legitimately key on;
//   5. jog, both families, into and away from a stop;
//   6. the property ("never MM_ENABLED with no steps to give") and a deliberate
//      tripwire on the fed-away characterization.
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

// Simulation step: 100 us of virtual time per update(), as test_thread_sync.
constexpr uint64_t kDt = 100;
// One realistic speed is enough here - this is a logic deadlock, not a timing
// one. 1200 PPS at PPR 1200 is 60 rpm.
constexpr int kPps = 1200;
// Round-trip amplitude in spindle pulses (2 revolutions).
constexpr int kK = 2400;

class EndstopDeadlockTest : public ::testing::Test {
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

  // Drive the spindle by `pulses` (signed) net encoder counts at kPps, calling
  // ls->update() every kDt. Identical in shape to test_thread_sync's version.
  void driveSpindle(int pulses) {
    const int dir = pulses >= 0 ? 1 : -1;
    const int target = pulses >= 0 ? pulses : -pulses;
    const double perStep = (double)kPps * (double)kDt / 1e6;
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

  // As driveSpindle, but reports the worst |following error| seen along the
  // way. A stall shows up here as unbounded growth even though the carriage
  // never moves, which is the diagnostic signature of the deadlock.
  float driveSpindleTrackingError(int pulses) {
    const int dir = pulses >= 0 ? 1 : -1;
    const int target = pulses >= 0 ? pulses : -pulses;
    const double perStep = (double)kPps * (double)kDt / 1e6;
    double carry = 0.0;
    int delivered = 0;
    float worst = 0.0f;
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
      const float e = fabsf(ls->getPositionError());
      if (e > worst) worst = e;
    }
    return worst;
  }

  // Hold the spindle still and pump update() until the carriage rests. This is
  // also what runs the deceleration ramp down to zero, which is what releases
  // the direction latch (leadscrew.cpp, the MM_DISABLED/MM_DECELLERATE branch).
  void settle(int maxSteps = 40000) {
    int stableFor = 0;
    int last = ls->getCurrentPosition();
    for (int i = 0; i < maxSteps; ++i) {
      advanceMockMicros(kDt);
      ls->update();
      const int now = ls->getCurrentPosition();
      if (now == last) {
        if (++stableFor > 1000) return;
      } else {
        stableFor = 0;
        last = now;
      }
    }
  }

  int establishSyncAt(LeadscrewStopPosition stop) {
    ls->setCurrentPosition(0);
    ls->setStopPosition(stop);
    EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC);
    return spindle->getCurrentPosition();
  }

  // Put the carriage ON the given stop, arrested and fully settled, exactly the
  // way a finished thread pass leaves it: thread out, re-arm the stop, thread
  // back into it, let the ramp run down.
  //
  // `pitch` sign selects the hand of the thread; `outward` is the spindle
  // direction that feeds AWAY from `stop`.
  void parkArrestedOn(LeadscrewStopPosition stop, float pitch, int outward) {
    ls->setTargetPitchMM(pitch);
    spindle->incrementCurrentPosition(500);  // non-trivial phase
    spindle->consumePosition();
    establishSyncAt(stop);
    ls->unsetStopPosition(stop);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(outward * kK);
    settle();

    ls->setStopPosition(stop, 0);
    driveSpindle(-outward * kK);
    settle();
  }

  // ---- helpers added for the fix's specification -------------------------

  // Run update() a FIXED number of times with the spindle held still.
  //
  // Not settle(): settle() returns as soon as the carriage has been stationary
  // for 1000 iterations, which is precisely the wrong tool for the "engaged but
  // idle" cases below. Those ask what the axis does when nothing has been asked
  // of IT yet - the operator has pressed ENABLE and has not started the spindle
  // - so the pump must keep going exactly BECAUSE the carriage is stationary.
  void pump(int updates) {
    for (int i = 0; i < updates; ++i) {
      advanceMockMicros(kDt);
      ls->update();
    }
  }

  // Everything the stall property needs from one drive, gathered in a single
  // pass so a scenario does not have to be run three times to be judged.
  struct DriveOutcome {
    int moved;              // net carriage travel over the drive, in pulses
    int maxPosition;        // furthest right the carriage ever got
    int minPosition;        // furthest left the carriage ever got
    float worstError;       // worst |following error| seen at any point
    GlobalMotionMode mode;  // the mode the axis was left in
    int updatesEngaged;     // update() calls that ran while still MM_ENABLED
    int updates;            // update() calls in total
  };

  // driveSpindle + driveSpindleTrackingError + the bookkeeping the stall
  // property needs. Same drive shape as both of them, so the three are
  // interchangeable and a scenario measured here is the same scenario the
  // existing tests drive.
  DriveOutcome driveAndWatch(int pulses) {
    const int dir = pulses >= 0 ? 1 : -1;
    const int target = pulses >= 0 ? pulses : -pulses;
    const double perStep = (double)kPps * (double)kDt / 1e6;
    const int start = ls->getCurrentPosition();
    DriveOutcome o{};
    o.maxPosition = start;
    o.minPosition = start;
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
      ++o.updates;
      if (gs->getMotionMode() == GlobalMotionMode::MM_ENABLED) ++o.updatesEngaged;
      const int p = ls->getCurrentPosition();
      if (p > o.maxPosition) o.maxPosition = p;
      if (p < o.minPosition) o.minPosition = p;
      const float e = fabsf(ls->getPositionError());
      if (e > o.worstError) o.worstError = e;
    }
    o.moved = ls->getCurrentPosition() - start;
    o.mode = gs->getMotionMode();
    return o;
  }

  // THE REALISTIC RIG, and the one the specification below is written against.
  //
  // parkArrestedOn() above deliberately unsets the stop to place the carriage,
  // which leaves the spindle-sync anchor UNSET - so syncArmed() is false and
  // update()'s re-sync gate never runs. That is not the machine the operator
  // has. On a real thread pass BOTH stops are set and the helix anchor is armed
  // at one of them, which means the gate DOES run, SS_UNSYNC/SS_SYNC cycles
  // over the pass, and the carriage parks on the stop exactly (measured: 0)
  // rather than two pulses past it (measured: -2 on the parkArrestedOn rig,
  // which is an artefact of that rig - see the tripwire test at the bottom).
  //
  // So: set both stops, anchor the helix at the one the pass will finish on,
  // thread OUT and back IN, and let the arrest and the ramp complete. What is
  // left is a machine sitting on its stop with a live anchor, which is the
  // state the operator's finger is over the ENABLE key in.
  //
  // `pitch` sign is the hand of the thread; `outward` is the spindle direction
  // that feeds AWAY from `stop`; `farStop` is where the other stop goes.
  void runPassAndArrestOn(LeadscrewStopPosition stop, float pitch, int outward,
                          int farStop) {
    ls->setTargetPitchMM(pitch);
    spindle->incrementCurrentPosition(500);  // non-trivial phase
    spindle->consumePosition();
    ls->setCurrentPosition(0);
    ls->setStopPosition(stop, 0);
    ls->setStopPosition(stop == LeadscrewStopPosition::LEFT
                            ? LeadscrewStopPosition::RIGHT
                            : LeadscrewStopPosition::LEFT,
                        farStop);
    ASSERT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_SYNC)
        << "the first stop placed at the carriage position arms the helix "
           "anchor; without it this rig is not the machine under test";

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    driveSpindle(outward * kK);   // the pass, away from `stop`
    driveSpindle(-outward * kK);  // and back into it
    settle();

    ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
        << "the pass must have arrested on its stop before the test begins";
    ASSERT_EQ(ls->getCurrentPosition(), 0) << "parked exactly on the stop";
    ASSERT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN)
        << "the settled ramp must have released the direction latch - that is "
           "the precondition the deadlock needs";
  }
};

// The four ways a synced pass can finish ON a stop: either stop, either hand of
// thread. The existing two stall tests are the first and last of these; the
// other two are added below, because nothing about the mechanism is specific to
// a hand and a fix that only mends the case in front of it is not a fix.
//
// `outward` is the SPINDLE direction that feeds the carriage AWAY from `stop`;
// `awaySign` is the CARRIAGE direction (in leadscrew pulses) that is away from
// it. `farStop` is where the other stop is placed for the realistic rig.
struct ThreadCase {
  const char* name;
  LeadscrewStopPosition stop;
  float pitch;
  int outward;
  int farStop;
  int awaySign;
};

const ThreadCase kCases[] = {
    {"right-hand thread (+pitch) arresting at the LEFT stop",
     LeadscrewStopPosition::LEFT, 0.25f, +1, 4000, +1},
    {"left-hand thread (-pitch) arresting at the LEFT stop",
     LeadscrewStopPosition::LEFT, -0.25f, -1, 4000, +1},
    {"right-hand thread (+pitch) arresting at the RIGHT stop",
     LeadscrewStopPosition::RIGHT, 0.25f, -1, -4000, -1},
    {"left-hand thread (-pitch) arresting at the RIGHT stop",
     LeadscrewStopPosition::RIGHT, -0.25f, +1, -4000, -1},
};

// Long enough that a working axis is unambiguously working: 3 * kK spindle
// pulses is 6 revolutions, which at the default geometry and 0.25 mm pitch is
// ~474 leadscrew pulses of demand. A stalled axis moves 0 of them.
constexpr int kLongDrive = kK * 3;
// Measured travel for kLongDrive when the axis is genuinely running (474);
// asserted with margin, since the point is "it ran a pass", not a pulse count.
constexpr int kLongDriveMinTravel = 400;
// A 2 s dwell with the axis engaged and the spindle stopped - the operator
// pressing ENABLE and then reaching for the spindle switch.
constexpr int kDwellUpdates = 20000;

// ---------------------------------------------------------------------------
// Baseline: the arrest itself works. Pinned so the fix cannot regress it.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, ArrivingAtTheStopUnderPowerArrests) {
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);

  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
      << "a thread pass running into its stop must disable the axis";
  EXPECT_EQ(gs->getThreadSyncState(), GlobalThreadSyncState::SS_UNSYNC);
  EXPECT_NEAR(ls->getCurrentPosition(), 0, 2) << "parked on the stop";
}

TEST_F(EndstopDeadlockTest, TheSettledArrestReleasesTheDirectionLatch) {
  // Not an end in itself - this is the PRECONDITION for the deadlock below, so
  // it is asserted separately. If this ever stops being true, the two stall
  // tests are no longer testing what their names say.
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);

  EXPECT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN)
      << "the ramp reaching zero is what clears the latch";
  EXPECT_FLOAT_EQ(ls->getLeadscrewSpeedPulsesPerSecond(), 0.0f);
}

// ---------------------------------------------------------------------------
// THE DEADLOCK. Expected to FAIL until Leadscrew::update() is fixed.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, ReEngagingWhileParkedOnTheLeftStopMustNotStall) {
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  ASSERT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);

  // The operator presses ENABLE without jogging off the stop first. This is
  // exactly ButtonPad's ToggleEngage on MM_DISABLED (buttonpad.cpp).
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  // The spindle carries on turning in the direction that feeds INTO the stop -
  // the carriage has nowhere to go.
  driveSpindle(-kK);

  // The axis must resolve to a mode that tells the truth. MM_ENABLED here is a
  // lie the panel then acts on: it reads CUTTING and locks the keypad out via
  // underPower(), which is what made this look like a firmware hang.
  EXPECT_NE(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "STALLED: engaged, parked on the stop, no steps possible and no way "
         "out - the panel shows CUTTING and locks itself out";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
      << "re-engaging into a stop the carriage is already on must arrest, the "
         "same as arriving at it under power";
}

TEST_F(EndstopDeadlockTest, ReEngagingWhileParkedOnTheRightStopMustNotStall) {
  // The mirror case: a left-hand thread arrests at the RIGHT stop.
  parkArrestedOn(LeadscrewStopPosition::RIGHT, -0.25f, +1);
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  ASSERT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(-kK);

  EXPECT_NE(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "STALLED at the right stop - same deadlock, other hand of thread";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
}

TEST_F(EndstopDeadlockTest, AStalledEngageDoesNotAccumulateUnboundedError) {
  // The diagnostic signature, asserted in its own right. While stalled, the
  // spindle keeps feeding m_expectedPosition and nothing consumes it, so the
  // following error grows without limit behind a screen that says CUTTING.
  // Whatever the axis does when re-engaged onto a stop, it must not do that -
  // an unbounded error is what saturated posError in the ESP32Encoder glitch
  // (CLAUDE.md, motion gotchas) and it is no more acceptable arrived at this
  // way.
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  const float worst = driveSpindleTrackingError(-kK);

  // kK spindle pulses at the test ratio is far more than a decelerating axis
  // could legitimately fall behind; anything approaching it means nothing is
  // consuming the error at all.
  EXPECT_LT(worst, (float)kK * 0.5f)
      << "following error ran away while the axis was stalled on the stop";
}

// ---------------------------------------------------------------------------
// What the direction guard is FOR. These must keep passing after the fix, and
// they are the reason "arrest whenever the latch is UNKNOWN" is the wrong fix.
// ---------------------------------------------------------------------------

// CHARACTERIZATION, not a requirement. Re-engaging while parked on a stop and
// feeding AWAY from it currently arrests rather than running the pass.
//
// This began as an assertion that the axis would feed away freely, and that was
// a GUESS about the intended behaviour, not something the code or the owner had
// said. It is recorded as observed instead, because probing it showed the
// arrest is legitimate on its own terms: during the sync wait at position 0 the
// latch re-acquires LEFT (dirBefore = -1, speed 0, error 0, ~454 updates in),
// so `hitLeftEndstop && m_currentDirection == LEFT` is genuinely true and the
// arrest is doing exactly what it says.
//
// Which is the useful half of the finding: the SAME re-latching that stops this
// case cannot happen in the two stall tests above, because there the axis wants
// to go LEFT and the left branch - the only place the latch is ever assigned -
// is blocked by !hitLeftEndstop. The latch stays UNKNOWN and nothing can end
// it. That asymmetry is the deadlock, and it is why the fix has to give the
// arrest a way to fire that does not depend on the latch.
//
// Whether arresting here is the RIGHT behaviour is a question for the owner: it
// is safe (it stops rather than stalls) but it means a pass started from the
// stop you just finished on needs a jog off first. Left as-is deliberately.
TEST_F(EndstopDeadlockTest, ParkedOnAStopAndFedAwayFromItCurrentlyArrests) {
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  driveSpindle(+kK);  // outward: away from the left stop

  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
      << "observed behaviour: the latch re-acquires LEFT during the sync wait "
         "and the arrest fires";
  EXPECT_NEAR(ls->getCurrentPosition(), 0, 2)
      << "the pass never leaves the stop";
}

TEST_F(EndstopDeadlockTest, StartingAPassFromTheOppositeStopDoesNotDisable) {
  // The case the arrest's own comment calls out: "starting at the opposite stop
  // does not immediately disable". Both stops are set and the carriage begins
  // on the right-hand one, threading left.
  ls->setTargetPitchMM(0.25f);
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -4000);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(-kK);  // feed LEFT, away from the right stop

  EXPECT_LT(ls->getCurrentPosition(), 0)
      << "a pass started at the far stop must travel";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "touching the stop behind you must not arrest the pass";
}


// ---------------------------------------------------------------------------
// THE DEADLOCK, COMPLETED. The two seed tests above cover one hand of thread at
// each stop; these are the other two. Expected to FAIL until the fix lands.
//
// They exist because the mechanism has nothing to do with the hand of the
// thread. m_currentDirection is a CARRIAGE direction, not a spindle one, and
// the sign of the pitch only decides which way the spindle has to turn to push
// the carriage into a given stop. A fix that mends "left stop, right-hand
// thread" and not this is a fix aimed at the reproduction rather than the bug.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, ReEngagingOnTheLeftStopWithALeftHandThreadMustNotStall) {
  // Negative pitch, so the spindle turns the other way to feed the carriage
  // left. Everything else is the seed test.
  parkArrestedOn(LeadscrewStopPosition::LEFT, -0.25f, -1);
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  ASSERT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(+kK);  // feeds INTO the left stop for a left-hand thread

  EXPECT_NE(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "STALLED at the left stop on a left-hand thread - the latch is a "
         "carriage direction, so the hand of the thread cannot matter";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
}

TEST_F(EndstopDeadlockTest, ReEngagingOnTheRightStopWithARightHandThreadMustNotStall) {
  parkArrestedOn(LeadscrewStopPosition::RIGHT, 0.25f, -1);
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  ASSERT_EQ(ls->getCurrentDirection(), LeadscrewDirection::UNKNOWN);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  driveSpindle(+kK);  // feeds INTO the right stop for a right-hand thread

  EXPECT_NE(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "STALLED at the right stop on a right-hand thread";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
}

// ---------------------------------------------------------------------------
// THE SAME DEADLOCK ON THE MACHINE THE OPERATOR HAS: both stops set, the helix
// anchor armed, a real out-and-back pass. Expected to FAIL until the fix lands.
//
// This matters more than the four tests above, for two reasons.
//
// First, it is reachable by the documented gesture and nothing else: finish a
// pass, press ENABLE without jogging clear. parkArrestedOn() has to unset a
// stop to place the carriage, which leaves the sync anchor UNSET and update()'s
// re-sync gate dormant; on a real pass the gate is live, and the stall has to
// be shown to survive that. It does - measured identically on both rigs, 0
// pulses of travel against ~472 pulses of banked following error.
//
// Second, it is the rig the "must not break" tests below share, so the pair
// "same setup, spindle turning the other way" is the whole specification in two
// tests: feed INTO the stop you are parked on and the axis must arrest; feed
// AWAY from it and the axis must run the pass. Any fix that cannot tell those
// two apart is wrong, in one direction or the other.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, AFinishedPassReEngagedIntoItsOwnStopMustNotStall) {
  for (const ThreadCase& c : kCases) {
    SCOPED_TRACE(c.name);
    buildRig();
    runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);

    // ENABLE, without jogging clear first. ButtonPad's ToggleEngage on
    // MM_DISABLED (src/buttonpad.cpp) is exactly this one line.
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    const DriveOutcome o = driveAndWatch(-c.outward * kLongDrive);

    EXPECT_NE(o.mode, GlobalMotionMode::MM_ENABLED)
        << "STALLED: engaged, parked on the stop, the spindle feeding into it, "
           "and not one pulse of travel possible. The panel reads CUTTING and "
           "underPower() locks the keypad out, so the operator cannot even "
           "disengage from the machine that is lying to them";
    EXPECT_EQ(o.mode, GlobalMotionMode::MM_DISABLED)
        << "re-engaging into the stop the carriage is already on must arrest, "
           "the same as arriving at it under power";
  }
}

TEST_F(EndstopDeadlockTest, ReEngagingIntoAStopMustNotBankUnboundedFollowingError) {
  // The diagnostic signature on the realistic rig, and a tighter bound than
  // AStalledEngageDoesNotAccumulateUnboundedError above can offer: on that rig
  // the whole drive can only ever produce kK * ratio = 157 pulses of error, so
  // its kK/2 = 1200 ceiling is never actually load-bearing.
  //
  // The numbers here are measured, not guessed. Feeding INTO the stop for
  // kLongDrive banks 472.4 pulses of following error today - the entire drive,
  // because nothing consumes any of it. Feeding AWAY over the same drive, with
  // the axis genuinely tracking, the worst excursion is 3.2 pulses. An axis
  // that arrests promptly sits between the two: the arrest pins
  // m_expectedPosition to m_currentPosition once the ramp reaches zero, so the
  // error stops growing within a few tens of milliseconds of the demand
  // appearing. 40 pulses is about half a second of lag at this feed - generous
  // for any reasonable arrest, and nowhere near 472.
  //
  // Why it must be bounded at all: an unbounded following error is what
  // saturated posError to -2^31 in the ESP32Encoder glitch (CLAUDE.md, motion
  // gotchas). Arriving at it this way is no more acceptable, and here it is
  // behind a screen that says CUTTING.
  buildRig();
  runPassAndArrestOn(kCases[0].stop, kCases[0].pitch, kCases[0].outward,
                     kCases[0].farStop);
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  const DriveOutcome o = driveAndWatch(-kCases[0].outward * kLongDrive);

  EXPECT_LT(o.worstError, 40.0f)
      << "the axis banked following error it can never work off; measured 472 "
         "pulses before the fix, 3.2 on the same rig when the axis is running";
}

TEST_F(EndstopDeadlockTest, EngagedAndIdleOnAStopThenFedIntoItMustArrest) {
  // The other order of events, and the one that separates "there is demand into
  // the stop" from "the axis happens to be sitting on a stop". The operator
  // presses ENABLE with the spindle stopped, waits, and only then starts the
  // spindle - turning it the way that feeds into the stop.
  //
  // Nothing may happen during the dwell (see the companion AWAY test, which is
  // the same two seconds followed by the other spindle direction and MUST run a
  // pass). The arrest becomes due at the moment the demand appears, not before.
  for (const ThreadCase& c : kCases) {
    SCOPED_TRACE(c.name);
    buildRig();
    runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    pump(kDwellUpdates);
    ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
        << "an engaged axis with a stopped spindle has been asked for nothing "
           "and must not arrest on that alone";

    const DriveOutcome o = driveAndWatch(-c.outward * kLongDrive);

    EXPECT_NE(o.mode, GlobalMotionMode::MM_ENABLED)
        << "STALLED after the dwell - same deadlock, reached by starting the "
           "spindle rather than by pressing ENABLE";
    EXPECT_EQ(o.mode, GlobalMotionMode::MM_DISABLED);
  }
}

// ---------------------------------------------------------------------------
// WHAT THE FIX MUST NOT BREAK. Every test from here down PASSES TODAY and must
// still pass afterwards.
//
// These are the reason "arrest whenever the direction latch is UNKNOWN" is the
// wrong fix, and so is "arrest whenever the axis is engaged, on a stop and
// cannot step". Both of those are true of an axis that is about to run a
// perfectly good pass: the latch is UNKNOWN every time the machine is at rest,
// and an engaged axis waiting for its spindle to be switched on cannot step
// either. The distinguishing fact is not the latch and not the stop - it is
// whether anything is currently ASKING the carriage to move into the stop it is
// touching.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, AFinishedPassReEngagedAndFedAwayFromItsStopStillRuns) {
  // The mirror of AFinishedPassReEngagedIntoItsOwnStopMustNotStall, and the
  // single most important fence in this file: identical setup, spindle turning
  // the other way. This is the ordinary next pass - back the tool out, wind on
  // another few thou, re-engage on the stop you finished on and cut away from
  // it. It works today (measured: 474 pulses of travel, 3.2 pulses of worst
  // following error, still MM_ENABLED at the end) and it is what an
  // over-eager arrest destroys.
  //
  // THE TRAP THIS FENCE EXISTS TO CATCH, measured on this rig, because it is
  // not visible from reading update() and it will bite the obvious fix:
  //
  //   THE SPINDLE VELOCITY ESTIMATE IS STALE AT THE MOMENT OF RE-ENGAGEMENT,
  //   AT FULL MAGNITUDE AND IN THE PREVIOUS PASS'S DIRECTION. Measured right
  //   after settle(): -1200 PPS, i.e. the inbound pass, undecayed. Spindle
  //   (both TestSpindle and ESPSpindle) only zeroes it after a whole second
  //   with no encoder pulses, and settle() finishes long before that.
  //
  // update()'s `positionError` is `positionErrorRaw + pulsesToTargetSpeed`, and
  // pulsesToTargetSpeed is derived entirely from that estimate. So for the
  // first fraction of a second of a pass fed AWAY from the stop, positionError
  // points INTO the stop - purely as feed-forward for a rotation that already
  // stopped. Any arrest test written against positionError therefore fires
  // here, disengages the axis before the cut starts, and fails this test.
  //
  // positionErrorRaw does not have that problem, and the separation is total,
  // not marginal. Sampled only on iterations where the carriage is actually
  // touching the stop:
  //
  //     fed AWAY:  worst raw error toward the stop  =   0.00 pulses
  //                (and the carriage is clear of the stop within 133
  //                 iterations, ~13 ms)
  //     fed INTO:  worst raw error toward the stop  = -78.74 pulses
  //                (and the carriage never gets clear at all)
  //
  // Identical in all four hand/stop combinations, mirrored in sign. Whatever
  // the fix keys on, it has to be a signal that looks like that pair.
  for (const ThreadCase& c : kCases) {
    SCOPED_TRACE(c.name);
    buildRig();
    runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    const DriveOutcome o = driveAndWatch(c.outward * kLongDrive);

    EXPECT_EQ(o.mode, GlobalMotionMode::MM_ENABLED)
        << "a pass fed AWAY from the stop it starts on must be allowed to run";
    EXPECT_GT(o.moved * c.awaySign, kLongDriveMinTravel)
        << "the carriage must actually travel away from the stop, not just "
           "stay nominally engaged";
    EXPECT_LT(o.worstError, 40.0f)
        << "and it must TRACK while it does - staying MM_ENABLED while falling "
           "further and further behind is the stall wearing a different hat";
  }
}

TEST_F(EndstopDeadlockTest, EngagedOnAStopWithTheSpindleStoppedThenFedAwayStillRuns) {
  // The same pass, begun the way an operator actually begins one: ENABLE first,
  // spindle second. For two whole seconds the axis is engaged, sitting on a
  // stop, with the direction latch UNKNOWN and no step it could possibly take.
  // That is the exact state the deadlock tests describe, and it is completely
  // legitimate here - the difference is only that the demand, when it arrives,
  // points away from the stop.
  //
  // A fix that arrests on "engaged and unable to step" kills this, and with it
  // the ability to start any pass from a stop.
  for (const ThreadCase& c : kCases) {
    SCOPED_TRACE(c.name);
    buildRig();
    runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);

    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    pump(kDwellUpdates);
    ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
        << "the axis disengaged itself during the dwell, before the spindle "
           "had been asked to do anything at all";

    const DriveOutcome o = driveAndWatch(c.outward * kLongDrive);

    EXPECT_EQ(o.mode, GlobalMotionMode::MM_ENABLED);
    EXPECT_GT(o.moved * c.awaySign, kLongDriveMinTravel);
  }
}

TEST_F(EndstopDeadlockTest, StartingAPassFromTheOppositeStopAfterADwellDoesNotDisable) {
  // StartingAPassFromTheOppositeStopDoesNotDisable above starts the spindle in
  // the same breath as ENABLE, so the window in which the axis is engaged, on a
  // stop and idle is only a handful of iterations long - short enough that a
  // fix which arrests on that state could plausibly slip past it. This is the
  // same test with two seconds of that window instead.
  ls->setTargetPitchMM(0.25f);
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -4000);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  pump(kDwellUpdates);
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "engaged and idle on the far stop is not a fault - it is the operator "
         "reaching for the spindle switch";

  driveSpindle(-kK);  // feed LEFT, away from the right stop

  EXPECT_LT(ls->getCurrentPosition(), 0)
      << "a pass started at the far stop must travel";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED)
      << "touching the stop behind you must not arrest the pass";
}

TEST_F(EndstopDeadlockTest, AwayFromItsStopTheAxisTracksForAWholePass) {
  // Guards the other failure shape a fix could introduce: arresting not at the
  // start of the pass but somewhere along it, because the carriage was near a
  // stop when some transient made the demand momentarily point at it. Twenty
  // revolutions of spindle, ~3150 pulses of travel, and the axis must be
  // engaged and in step the whole way.
  buildRig();
  runPassAndArrestOn(LeadscrewStopPosition::LEFT, 0.25f, +1, 4000);
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  const DriveOutcome o = driveAndWatch(+kK * 20);

  EXPECT_EQ(o.mode, GlobalMotionMode::MM_ENABLED);
  EXPECT_GT(o.moved, 3000) << "the whole pass must run, not the first inch";
  EXPECT_LT(o.worstError, 40.0f);
}

TEST_F(EndstopDeadlockTest, JoggingOffTheStopRestoresANormalReEngage) {
  // The workaround the operator has today, and the recovery path after the fix
  // arrests them. It must keep working: jog clear of the stop, re-engage, and
  // the axis follows the spindle normally without arresting on a stop it is no
  // longer touching.
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);

  gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
  pump(2000);
  gs->setMotionMode(GlobalMotionMode::MM_DECELLERATE);
  settle();
  ASSERT_GT(ls->getCurrentPosition(), 100) << "the jog must have got clear";
  ASSERT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);

  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
  const DriveOutcome o = driveAndWatch(-kK);

  EXPECT_EQ(o.mode, GlobalMotionMode::MM_ENABLED)
      << "clear of the stop, a pass fed toward it must run until it arrives";
  EXPECT_LT(o.moved, -100) << "and must actually travel toward the stop";
}

// ---------------------------------------------------------------------------
// JOG. Both jog families appear in the arrest condition and in the stepping
// branches, and they are the most likely casualty of a careless fix - the
// arrest condition is a single `if` and it is very tempting to "tidy" it.
//
// Note the asymmetry in leadscrew.cpp that these pin: the powered run
// (MM_JOG_LEFT/RIGHT, the display's "RETURNING") is listed in the arrest and is
// gated by the stop in the stepping branches, whereas the dead-man jog
// (MM_INTERACTIVE_JOG_*, the display's "JOG <-") is in NEITHER - its stepping
// arm is OR'd in outside the `&& !hitEndstop`. That is not an oversight to be
// swept up; see the characterization test at the end of this block.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, PoweredRunIntoAStopStillArrests) {
  {
    SCOPED_TRACE("run LEFT into the left stop");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, -400);
    gs->setMotionMode(GlobalMotionMode::MM_JOG_LEFT);
    pump(40000);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
    EXPECT_NEAR(ls->getCurrentPosition(), -400, 5);
  }
  {
    SCOPED_TRACE("run RIGHT into the right stop");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 400);
    gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
    pump(40000);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
    EXPECT_NEAR(ls->getCurrentPosition(), 400, 5);
  }
}

TEST_F(EndstopDeadlockTest, PoweredRunAwayFromAStopTheCarriageSitsOnStillRuns) {
  // The counterpart, and the thing the arrest's direction guard buys for jog:
  // the carriage is ON a stop and the operator asks it to run the other way.
  // This must work - it is how you get off a stop at all, and after the fix it
  // is the recovery from the arrest the deadlock tests demand.
  {
    SCOPED_TRACE("sitting on the left stop, run RIGHT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_JOG_RIGHT)
        << "the run was arrested by the stop it was leaving";
    EXPECT_GT(ls->getCurrentPosition(), 1000);
  }
  {
    SCOPED_TRACE("sitting on the right stop, run LEFT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_JOG_LEFT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_JOG_LEFT);
    EXPECT_LT(ls->getCurrentPosition(), -1000);
  }
}

TEST_F(EndstopDeadlockTest, PoweredRunFromOneStopToTheOtherStillArrestsAtTheFarStop) {
  // Both stops set, carriage on the left one, run right: it must leave the stop
  // it is on and arrest on the one it arrives at. One test, both halves of the
  // guard.
  buildRig();
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 4000);
  gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
  pump(40000);

  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  EXPECT_NEAR(ls->getCurrentPosition(), 4000, 5);
}

TEST_F(EndstopDeadlockTest, InteractiveJogAwayFromAStopTheCarriageSitsOnStillRuns) {
  {
    SCOPED_TRACE("sitting on the left stop, dead-man jog RIGHT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT);
    EXPECT_GT(ls->getCurrentPosition(), 1000);
  }
  {
    SCOPED_TRACE("sitting on the right stop, dead-man jog LEFT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT);
    EXPECT_LT(ls->getCurrentPosition(), -1000);
  }
}

// CHARACTERIZATION, not a requirement - but a load-bearing fence all the same.
//
// The dead-man jog IGNORES stops completely. MM_INTERACTIVE_JOG_* is absent
// from the arrest condition, and its arm of each stepping branch is OR'd in
// OUTSIDE the `&& !hitEndstop` guard, so the carriage drives straight through.
// Measured here: a stop at -400, and the carriage ends up at about -38000,
// still in MM_INTERACTIVE_JOG_LEFT, with no arrest at any point.
//
// Recorded rather than asserted-as-correct because nothing in the code says in
// so many words that it is deliberate. But it is very probably deliberate, and
// that is why this test is here: the dead-man jog is the operator's only way to
// move the carriage PAST a stop (the powered run and the feed both refuse), and
// it is a hold-to-move gesture with the operator's finger on the key and their
// eyes on the tool. A fix that "completes" the arrest condition by adding the
// interactive modes to it - which is a one-line change and looks like tidying -
// takes that away and strands anyone who has set a stop in the wrong place.
//
// If this test starts failing, the change was probably not intended. If it WAS
// intended, that is an owner's decision about how the machine behaves, not a
// side effect of fixing the deadlock.
TEST_F(EndstopDeadlockTest, InteractiveJogDrivesStraightThroughAStop) {
  buildRig();
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, -400);
  gs->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT);
  pump(40000);

  EXPECT_LT(ls->getCurrentPosition(), -400)
      << "observed behaviour: the dead-man jog is not gated by the stop";
  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT)
      << "observed behaviour: and it is not in the arrest condition either";
}

// ---------------------------------------------------------------------------
// HOLD-JOG (issue #11, round 2): GlobalMotionMode::MM_HOLD_JOG_LEFT/RIGHT.
//
// NOT IMPLEMENTED in Leadscrew::update() yet. Today the new modes match
// neither direction-selection branch (leadscrew.cpp ~483/492) nor the
// endstop-arrest condition (~378-379) nor the shouldStop speed switch
// (~688-707), so m_currentDirection never leaves UNKNOWN and the axis simply
// never moves under them. Every test below asserts the CORRECT target
// behaviour and is EXPECTED TO FAIL until the implementer wires the new
// modes into all three places - see the report for the precise scope:
//   1. the endstop-arrest condition: arrest like MM_JOG_* (equality, both
//      directions);
//   2. both direction-selection branches, INSIDE the `&& !hitEndstop` gate
//      like MM_JOG_* - NOT OR'd in outside it like MM_INTERACTIVE_JOG_*,
//      which is how the dead-man jog gets to ignore the stop;
//   3. the shouldStop switch: the MM_INTERACTIVE_JOG_* speed-cap formula
//      (jogSpeedPps() * getJogSpeed()) combined with the MM_JOG_* endstop
//      terms (goingToHitLeftEndstop / goingToHitRightEndstop) - no existing
//      case is this combination.
//
// The arrest tests mirror PoweredRunIntoAStopStillArrests /
// PoweredRunAwayFromAStopTheCarriageSitsOnStillRuns /
// PoweredRunFromOneStopToTheOtherStillArrestsAtTheFarStop above, with
// MM_HOLD_JOG_LEFT/RIGHT in place of MM_JOG_LEFT/RIGHT - the new mode must
// behave exactly like the powered run for arrest purposes, differing only in
// speed and in how UiState reaches it (Hold vs Click).
// ---------------------------------------------------------------------------

// RAII so a multiplier changed for one test can never leak into another -
// GlobalState::getInstance() is a true singleton and persists across every
// TEST_F case in this binary.
struct JogSpeedGuard {
  GlobalState* gs;
  int orig;
  explicit JogSpeedGuard(GlobalState* g) : gs(g), orig(g->getJogIndex()) {}
  ~JogSpeedGuard() {
    while (gs->getJogIndex() < orig) gs->incJogSpeed();
    while (gs->getJogIndex() > orig) gs->decJogSpeed();
  }
};

TEST_F(EndstopDeadlockTest, HoldJogIntoAStopStillArrests) {
  {
    SCOPED_TRACE("hold-jog LEFT into the left stop");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, -400);
    gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_LEFT);
    pump(40000);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
        << "the hold-jog must arrest at the stop the way MM_JOG_LEFT does";
    EXPECT_NEAR(ls->getCurrentPosition(), -400, 5);
  }
  {
    SCOPED_TRACE("hold-jog RIGHT into the right stop");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 400);
    gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_RIGHT);
    pump(40000);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED)
        << "the hold-jog must arrest at the stop the way MM_JOG_RIGHT does";
    EXPECT_NEAR(ls->getCurrentPosition(), 400, 5);
  }
}

TEST_F(EndstopDeadlockTest, HoldJogAwayFromAStopTheCarriageSitsOnStillRuns) {
  {
    SCOPED_TRACE("sitting on the left stop, hold-jog RIGHT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_RIGHT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_HOLD_JOG_RIGHT)
        << "the jog was arrested by the stop it was leaving";
    EXPECT_GT(ls->getCurrentPosition(), 1000);
  }
  {
    SCOPED_TRACE("sitting on the right stop, hold-jog LEFT");
    buildRig();
    ls->setCurrentPosition(0);
    ls->setStopPosition(LeadscrewStopPosition::RIGHT, 0);
    gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_LEFT);
    pump(kDwellUpdates);
    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_HOLD_JOG_LEFT);
    EXPECT_LT(ls->getCurrentPosition(), -1000);
  }
}

TEST_F(EndstopDeadlockTest,
       HoldJogFromOneStopToTheOtherStillArrestsAtTheFarStop) {
  buildRig();
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::LEFT, 0);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 4000);
  gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_RIGHT);
  pump(40000);

  EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_DISABLED);
  EXPECT_NEAR(ls->getCurrentPosition(), 4000, 5);
}

// ---------------------------------------------------------------------------
// SPEED (issue #11, round 2 - the owner's corrected requirement). UiState
// cannot see speed, only intents, so this is the only level that can pin it.
// The multiplier is set away from its 1.0 default (jogSpeeds[] top index) so
// a mode that ignores it cannot accidentally pass by coincidence.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest,
       HoldJogRightReachesTheJogSpeedMultiplierNotThePlainJogSpeed) {
  buildRig();
  JogSpeedGuard guard(gs);
  gs->decJogSpeed();
  gs->decJogSpeed();
  gs->decJogSpeed();  // top index (1.0) down to jogSpeeds[2] == 0.1
  ASSERT_FLOAT_EQ(gs->getJogSpeed(), 0.1f);

  ls->setCurrentPosition(0);  // both stops unset - nothing to arrest on yet
  gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_RIGHT);
  pump(kDwellUpdates);

  const float expected = derived->jogSpeedPps() * gs->getJogSpeed();
  EXPECT_NEAR(ls->getLeadscrewSpeedPulsesPerSecond(), expected,
              expected * 0.05f + 1.0f)
      << "the hold-jog must cruise at jogSpeedPps() * getJogSpeed(), the same "
         "speed MM_INTERACTIVE_JOG_RIGHT uses, NOT the plain jogSpeedPps() "
         "the Click-run (MM_JOG_RIGHT) uses";
  EXPECT_LT(ls->getLeadscrewSpeedPulsesPerSecond(), derived->jogSpeedPps())
      << "must be slower than the unmultiplied jog speed at this multiplier";
}

TEST_F(EndstopDeadlockTest,
       HoldJogLeftReachesTheJogSpeedMultiplierNotThePlainJogSpeed) {
  buildRig();
  JogSpeedGuard guard(gs);
  gs->decJogSpeed();
  gs->decJogSpeed();
  gs->decJogSpeed();
  ASSERT_FLOAT_EQ(gs->getJogSpeed(), 0.1f);

  ls->setCurrentPosition(0);
  gs->setMotionMode(GlobalMotionMode::MM_HOLD_JOG_LEFT);
  pump(kDwellUpdates);

  const float expected = derived->jogSpeedPps() * gs->getJogSpeed();
  EXPECT_NEAR(ls->getLeadscrewSpeedPulsesPerSecond(), expected,
              expected * 0.05f + 1.0f)
      << "same as the RIGHT case, mirrored";
  EXPECT_LT(ls->getLeadscrewSpeedPulsesPerSecond(), derived->jogSpeedPps());
}

// Regression pins: the new mode must not change what MM_JOG_* or
// MM_INTERACTIVE_JOG_* already do. Both of these pass today - they exist so
// a fix that reaches for the wrong case label (or merges the two families'
// speed formulas) is caught here rather than only in the two tests above.
TEST_F(EndstopDeadlockTest,
       JogRightSpeedStaysUnmultipliedRegardlessOfTheJogSpeedSetting) {
  buildRig();
  JogSpeedGuard guard(gs);
  gs->decJogSpeed();
  gs->decJogSpeed();
  gs->decJogSpeed();
  ASSERT_FLOAT_EQ(gs->getJogSpeed(), 0.1f);

  ls->setCurrentPosition(0);
  gs->setMotionMode(GlobalMotionMode::MM_JOG_RIGHT);
  pump(kDwellUpdates);

  EXPECT_NEAR(ls->getLeadscrewSpeedPulsesPerSecond(), derived->jogSpeedPps(),
              derived->jogSpeedPps() * 0.02f + 1.0f)
      << "regression: the Click-run's speed must stay unmultiplied";
}

TEST_F(EndstopDeadlockTest,
       InteractiveJogRightSpeedStaysAtTheMultiplier) {
  buildRig();
  JogSpeedGuard guard(gs);
  gs->decJogSpeed();
  gs->decJogSpeed();
  gs->decJogSpeed();
  ASSERT_FLOAT_EQ(gs->getJogSpeed(), 0.1f);

  ls->setCurrentPosition(0);
  gs->setMotionMode(GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT);
  pump(kDwellUpdates);

  // Wider tolerance than the plain-jogSpeedPps() regression test above:
  // measured, the discrete deceleration ramp settles into a saw-tooth a few
  // percent below the cap rather than converging on it exactly (observed
  // ~1224.8 against a 1259.84 cap, ~2.8% low).
  const float expected = derived->jogSpeedPps() * gs->getJogSpeed();
  EXPECT_NEAR(ls->getLeadscrewSpeedPulsesPerSecond(), expected,
              expected * 0.05f + 1.0f)
      << "regression: the dead-man jog's multiplier must be unaffected by "
         "the new mode's introduction";
}

// ---------------------------------------------------------------------------
// THE PROPERTY, stated once over everything above.
// ---------------------------------------------------------------------------

// An axis left in MM_ENABLED is telling the panel it is CUTTING, and UiState's
// underPower() takes it at its word: the knob, the stop edits and the menu all
// go dead. So MM_ENABLED carries an obligation - the axis must be in a position
// to act on what the spindle asks of it.
//
// Expressed as a property rather than a list of cases, because the list of ways
// to arrive at the bad state is exactly what nobody enumerated the first time:
//
//     if the axis is still MM_ENABLED at the end of a drive, then either it
//     moved, or nothing was being asked of it.
//
// "Nothing was being asked of it" is the following error staying near zero -
// that is what an idle engaged axis looks like (measured: 0.00). A stall is the
// third combination, and only the third: engaged, zero travel, and hundreds of
// pulses of demand banked up with nothing consuming them (measured: 0 pulses of
// travel against 472 of error). Arresting is always permitted by this property;
// it says nothing about axes that are not MM_ENABLED, because those are telling
// the truth whatever else is wrong with them.
//
// The scenarios run below are every INTO/AWAY combination on the realistic rig,
// with and without the two-second engaged dwell, plus the dwell on its own.
// Before the fix the eight INTO scenarios violate it and the rest satisfy it.
TEST_F(EndstopDeadlockTest, NoScenarioLeavesTheAxisEngagedWithNoStepsToGive) {
  for (const ThreadCase& c : kCases) {
    for (int dwell = 0; dwell < 2; ++dwell) {
      for (int into = 0; into < 2; ++into) {
        SCOPED_TRACE(::testing::Message()
                     << c.name << (dwell ? ", after a dwell" : "")
                     << (into ? ", fed INTO the stop" : ", fed AWAY from the stop"));
        buildRig();
        runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);
        gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
        if (dwell) pump(kDwellUpdates);

        const DriveOutcome o =
            driveAndWatch((into ? -c.outward : c.outward) * kLongDrive);

        const bool engaged = (o.mode == GlobalMotionMode::MM_ENABLED);
        const bool moved = (o.moved != 0);
        const bool demanded = (o.worstError > 40.0f);
        EXPECT_FALSE(engaged && !moved && demanded)
            << "the axis is engaged, has not moved a pulse, and has banked "
            << o.worstError
            << " pulses of following error - it is stalled in MM_ENABLED, "
               "which is the one state the panel cannot be got out of";
      }
    }
  }

  // And the boundary the property has to tolerate: engaged, on a stop, nothing
  // asked. Zero travel here is correct, and the error must stay at zero to
  // prove it really is "nothing asked" and not a stall in slow motion.
  for (const ThreadCase& c : kCases) {
    SCOPED_TRACE(::testing::Message() << c.name << ", engaged and idle");
    buildRig();
    runPassAndArrestOn(c.stop, c.pitch, c.outward, c.farStop);
    gs->setMotionMode(GlobalMotionMode::MM_ENABLED);
    pump(kDwellUpdates);

    EXPECT_EQ(gs->getMotionMode(), GlobalMotionMode::MM_ENABLED);
    EXPECT_EQ(ls->getCurrentPosition(), 0);
    EXPECT_NEAR(ls->getPositionError(), 0.0f, 1.0f);
  }
}

// ---------------------------------------------------------------------------
// TRIPWIRE for the characterization test above. Deliberately fragile.
// ---------------------------------------------------------------------------

// ParkedOnAStopAndFedAwayFromItCurrentlyArrests records that on the
// parkArrestedOn() rig, feeding away from the stop arrests. Its two assertions
// - MM_DISABLED, and the carriage within 2 pulses of the stop - are ALSO
// satisfied by an axis that arrests on the very first update, so a fix could
// change that behaviour completely without failing it. This test makes the
// difference visible.
//
// What actually happens today, measured update by update: the carriage is
// parked at -2 (two pulses past the stop at 0 - an artefact of that rig, which
// places the stop while the carriage is elsewhere and lets it overshoot on the
// way back; the realistic runPassAndArrestOn() rig parks at exactly 0). On
// ENABLE the demand climbs away from the stop, the axis steps clear to +2, its
// enormous acceleration overshoots the tiny 78 pps feed, the error inverts, it
// steps back LEFT - and now the latch says LEFT while the carriage is back on
// the stop, so the arrest fires legitimately. 454 iterations, 45 ms.
//
// So the arrest in that test is not the deadlock and not a bug; it is a hunting
// oscillation next to a stop, and it is a rig artefact rather than a property
// of the machine - note that the SAME gesture on the realistic rig
// (AFinishedPassReEngagedAndFedAwayFromItsStopStillRuns) runs the pass.
//
// THIS TEST WILL FAIL IF THE FIX ARRESTS ON THE FIRST UPDATE INSTEAD - which
// several reasonable fixes will, because at that first update the spindle's
// velocity estimate is still the stale inbound one and the demand momentarily
// points INTO the stop. That is very likely an improvement (it stops sooner and
// for a comprehensible reason). It is not a regression. But it is a change to
// how the machine behaves in a case the owner has already looked at once, so it
// must be a decision, not a silent side effect: if this fails, confirm the new
// behaviour is wanted and rewrite the two characterization tests together.
TEST_F(EndstopDeadlockTest, FedAwayTheAxisLeavesTheStopBeforeHuntingBackOntoIt_Tripwire) {
  parkArrestedOn(LeadscrewStopPosition::LEFT, 0.25f, +1);
  ASSERT_EQ(ls->getCurrentPosition(), -2)
      << "the parkArrestedOn rig parks two pulses past the stop; if that has "
         "changed, the hunt this test describes has changed with it";
  gs->setMotionMode(GlobalMotionMode::MM_ENABLED);

  const DriveOutcome o = driveAndWatch(+kK);  // outward, away from the stop

  EXPECT_EQ(o.mode, GlobalMotionMode::MM_DISABLED)
      << "unchanged from ParkedOnAStopAndFedAwayFromItCurrentlyArrests";
  EXPECT_GT(o.maxPosition, 0)
      << "the axis is expected to get CLEAR of the stop first (measured +2) and "
         "arrest only when the hunt brings it back onto it. Failing here means "
         "the fix now arrests immediately - read this test's comment before "
         "changing the number";
  EXPECT_GT(o.updatesEngaged, 100)
      << "measured 454 iterations engaged before the arrest; an arrest inside "
         "the first handful of iterations is a different behaviour, not a "
         "tighter version of this one";
}

// ---------------------------------------------------------------------------
// ISSUE #13: the shouldStop switch (leadscrew.cpp, ~line 723) has no
// `default:` and no `case MM_UNSET:`. MM_UNSET (= 0) is the only
// GlobalMotionMode value it misses.
//
// GlobalState's constructor always sets MM_DISABLED and nothing in lib/ or
// src/ ever writes MM_UNSET, so this is latent rather than live. But
// GlobalState::setMotionMode() takes the raw enum, so a test can put the
// machine there directly - and the path, once there, is not structurally
// dead: jogMode is false for MM_UNSET (MM_UNSET & MMF_JOG == 0), so it is
// treated exactly like MM_ENABLED for m_expectedPosition accumulation and
// for direction selection (leadscrew.cpp ~518-538, the ordinary
// positionError-driven branches), but it matches none of the spelled-out
// mode equalities in the endstop-arrest block above the switch (~378-471)
// and none of the switch's case labels. Reviewed at the object-code level
// (objdump on leadscrew.cpp.o): gcc resolves the uninitialised `shouldStop`
// to false, i.e. the ACCELERATE branch - a carriage that runs to its
// mechanical limit at increasing speed with the stops ignored.
//
// Reusing this file's rig rather than a new suite, for the same reason the
// HOLD_JOG section above does: the 100us-step clock here is the only one in
// the repo that can resolve a steady-state PPS (test_leadscrew.cpp's pump()
// forces one step per 100ms call regardless of speed, which cannot show
// "climbing toward max" versus "held near zero"). The bug itself is
// unrelated to the re-engage deadlock the rest of the file chases.
//
// WHAT'S PINNED HERE:
//   1. the core case - MM_UNSET must not accelerate;
//   2. stops are honoured - a stop set while MM_UNSET is active must not be
//      run through.
//
// WHAT IS DELIBERATELY NOT RE-PINNED: every mode the switch already has a
// case for. A `default:` (or an explicit `case MM_UNSET:`) cannot change the
// behaviour of an already-matched case label - C++ switch dispatch tries the
// specific labels first regardless of what else is in the switch - so the
// existing suites are the regression fence for that, for free:
//   MM_DISABLED / MM_DECELLERATE  - ArrivingAtTheStopUnderPowerArrests,
//     TheSettledArrestReleasesTheDirectionLatch,
//     JoggingOffTheStopRestoresANormalReEngage (this file);
//     DisabledModeDoesNotMove (test_leadscrew.cpp).
//   MM_ENABLED                    - the whole "THE DEADLOCK" and "MUST NOT
//     BREAK" blocks above (this file); EnabledModeFollowsSpindleWhenSynced,
//     NegativePitchReversesTravelDirection (test_leadscrew.cpp).
//   MM_JOG_LEFT / MM_JOG_RIGHT     - PoweredRunIntoAStopStillArrests,
//     PoweredRunAwayFromAStopTheCarriageSitsOnStillRuns,
//     PoweredRunFromOneStopToTheOtherStillArrestsAtTheFarStop,
//     JogRightSpeedStaysUnmultipliedRegardlessOfTheJogSpeedSetting (this
//     file); JogRightAdvancesOneStepPerUpdate, JogLeftRetreatsOneStepPerUpdate,
//     JogIntoRightEndstopStopsAndDisables (test_leadscrew.cpp).
//   MM_INTERACTIVE_JOG_LEFT/RIGHT  - InteractiveJogAwayFromAStopTheCarriageSitsOnStillRuns,
//     InteractiveJogDrivesStraightThroughAStop,
//     InteractiveJogRightSpeedStaysAtTheMultiplier (this file).
//   MM_HOLD_JOG_LEFT/RIGHT         - HoldJogIntoAStopStillArrests,
//     HoldJogAwayFromAStopTheCarriageSitsOnStillRuns,
//     HoldJogFromOneStopToTheOtherStillArrestsAtTheFarStop,
//     HoldJogRightReachesTheJogSpeedMultiplierNotThePlainJogSpeed,
//     HoldJogLeftReachesTheJogSpeedMultiplierNotThePlainJogSpeed (this file).
// Running this file (and test_leadscrew.cpp) after the fix is what proves
// none of those regressed; nothing new is added for them here.
// ---------------------------------------------------------------------------

TEST_F(EndstopDeadlockTest, UnsetMotionModeDoesNotAccelerateTowardMaxSpeed) {
  // No stops here - this pin is about the switch alone; the next test covers
  // the endstop. Enough spindle motion to latch a direction and put real
  // demand behind it, exactly as EnabledModeFollowsSpindleWhenSynced
  // (test_leadscrew.cpp) does for MM_ENABLED - jogMode is false for MM_UNSET
  // too, so the accumulation into m_expectedPosition is identical.
  ls->setTargetPitchMM(0.25f);
  ls->setCurrentPosition(0);
  gs->setMotionMode(GlobalMotionMode::MM_UNSET);

  driveSpindle(kK);  // latches a direction and banks real positionError
  settle();          // give a working ramp every chance to reach cruise and
                      // come back down; a broken one never stabilises and
                      // settle() falls through to its iteration cap instead

  EXPECT_LT(ls->getLeadscrewSpeedPulsesPerSecond(), 5.0f)
      << "an unrecognised motion mode must not accelerate the axis - pre-fix "
         "this climbs toward and holds at leadscrewMaxSpeedPps() ("
      << derived->leadscrewMaxSpeedPps()
      << " pps), because the shouldStop switch has no case for MM_UNSET and "
         "gcc resolves the uninitialised bool to false, the accelerate "
         "branch (issue #13)";
}

TEST_F(EndstopDeadlockTest, UnsetMotionModeDoesNotRunThroughASetStop) {
  // A real SET stop, close enough that a working axis could not possibly
  // miss it, and a drive long enough that a tracking axis would travel well
  // past it (kK*3 is kLongDrive - a working MM_ENABLED axis covers ~474
  // pulses over this same drive, see kLongDriveMinTravel above).
  ls->setTargetPitchMM(0.25f);
  ls->setCurrentPosition(0);
  ls->setStopPosition(LeadscrewStopPosition::RIGHT, 50);
  gs->setMotionMode(GlobalMotionMode::MM_UNSET);

  driveSpindle(kK * 3);
  settle();

  EXPECT_LE(ls->getCurrentPosition(), 55)
      << "the carriage ran through a SET stop while MM_UNSET - the "
         "endstop-arrest block (leadscrew.cpp ~378-471) is a list of "
         "spelled-out mode equalities that does not include MM_UNSET, and "
         "with the shouldStop switch also missing a case for it the broken "
         "accelerate branch keeps pulsing regardless of the stop. Measured "
         "position: "
      << ls->getCurrentPosition();
}

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
