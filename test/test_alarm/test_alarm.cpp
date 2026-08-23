// Host tests for the stepper-alarm state machine (lib/alarm/alarmmonitor.h).
//
// Everything the feature promises the operator is decided in this class, so it
// is all pinned here: that a fault LATCHES (a flicker still puts the modal up
// and still costs the sync), that a glitch shorter than the debounce does not,
// that OK produces a real one-second ENA pulse and not a mode, that a clear
// attempt against a fault which is still present fails visibly instead of
// silently, and that ENA is never left high on any path out of the machine.

#include <gmock/gmock.h>

#include <ostream>

#include "alarmmonitor.h"

std::ostream& operator<<(std::ostream& os, AlarmState s) {
  switch (s) {
    case AlarmState::Clear: return os << "State::Clear";
    case AlarmState::Alarm: return os << "State::Alarm";
    case AlarmState::Clearing: return os << "State::Clearing";
  }
  return os << "State::<?>";
}

namespace {

// Copies, so gtest never odr-uses the in-class constants by reference.
const unsigned long kDebounce = AlarmMonitor::kDebounceMs;
const unsigned long kPulse = AlarmMonitor::kEnaPulseMs;
const unsigned long kSettle = AlarmMonitor::kSettleMs;

// The monitor plus a virtual millisecond clock, sampled at a fixed period the
// way the alarm task in src/alarm.cpp does. Every test is explicit about how
// much time passed and what the line was doing while it did.
class Rig {
 public:
  Rig() : m_now(1000) {}

  // Hold the input at `asserted` for `ms`, sampling every kPeriodMs.
  void hold(bool asserted, unsigned long ms) {
    const unsigned long end = m_now + ms;
    while (m_now < end) {
      m_now += kPeriodMs;
      m_alarm.update(asserted, m_now);
    }
  }

  // One sample, no time passing beyond the sampling period itself.
  void sample(bool asserted) { hold(asserted, kPeriodMs); }

  AlarmMonitor& a() { return m_alarm; }

  // The alarm task's sampling period. Kept small relative to kDebounceMs so a
  // test that holds a level "for the debounce" really does cross it.
  static const unsigned long kPeriodMs = 5;

 private:
  AlarmMonitor m_alarm;
  unsigned long m_now;
};

const unsigned long Rig::kPeriodMs;

// --- Resting state ---------------------------------------------------------

TEST(AlarmMonitor, StartsClearWithTheDriversEnabled) {
  Rig r;
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().active());
  EXPECT_FALSE(r.a().enaAsserted());
  EXPECT_FALSE(r.a().consumeTrip());
  EXPECT_EQ(r.a().trips(), 0u);
}

TEST(AlarmMonitor, AHealthyLineNeverTrips) {
  Rig r;
  r.hold(false, 10000);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().consumeTrip());
  EXPECT_EQ(r.a().trips(), 0u);
}

// --- Debounce --------------------------------------------------------------

TEST(AlarmMonitor, AGlitchShorterThanTheDebounceIsIgnored) {
  Rig r;
  r.hold(true, kDebounce - Rig::kPeriodMs);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  r.hold(false, 1000);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_EQ(r.a().trips(), 0u);
}

TEST(AlarmMonitor, AnAssertionThatPersistsPastTheDebounceLatches) {
  Rig r;
  r.hold(true, kDebounce + Rig::kPeriodMs);
  EXPECT_EQ(r.a().state(), AlarmState::Alarm);
  EXPECT_TRUE(r.a().active());
  EXPECT_EQ(r.a().trips(), 1u);
}

// The reason the filter exists at all: the ALM pair runs the length of the loom
// beside the step and direction pairs, and a conducted spike must not stop a
// cut. Sampled as a dirty period rather than as one clean pulse.
TEST(AlarmMonitor, RepeatedShortGlitchesNeverAccumulateIntoATrip) {
  Rig r;
  for (int i = 0; i < 50; i++) {
    r.hold(true, Rig::kPeriodMs * 2);
    r.hold(false, Rig::kPeriodMs * 2);
  }
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_EQ(r.a().trips(), 0u);
}

// --- The latch -------------------------------------------------------------

TEST(AlarmMonitor, TheLatchSurvivesTheInputGoingAway) {
  Rig r;
  r.hold(true, kDebounce * 2);
  ASSERT_EQ(r.a().state(), AlarmState::Alarm);
  // The driver releases its alarm output by itself - a momentary fault. The
  // machine still stopped, so the operator still has to be told.
  r.hold(false, 5000);
  EXPECT_EQ(r.a().state(), AlarmState::Alarm);
  EXPECT_TRUE(r.a().active());
  // ...and the input readout says the fault itself is gone, which is what lets
  // the modal offer OK rather than "still faulted".
  EXPECT_FALSE(r.a().inputAsserted());
}

TEST(AlarmMonitor, TheTripEdgeIsReportedExactlyOnce) {
  Rig r;
  r.hold(true, kDebounce * 2);
  EXPECT_TRUE(r.a().consumeTrip());
  EXPECT_FALSE(r.a().consumeTrip());
  r.hold(true, 5000);
  EXPECT_FALSE(r.a().consumeTrip());
}

TEST(AlarmMonitor, DriversStayEnabledWhileMerelyLatched) {
  Rig r;
  r.hold(true, kDebounce * 2);
  ASSERT_EQ(r.a().state(), AlarmState::Alarm);
  r.hold(true, 3000);
  // ENA is the RESET line, not an inhibit: holding it high would make the
  // driver impossible to reset and would fight SW1.
  EXPECT_FALSE(r.a().enaAsserted());
}

// --- Clearing --------------------------------------------------------------

TEST(AlarmMonitor, OkPulsesEnaForTheFullPulseLengthThenReleasesIt) {
  Rig r;
  r.hold(true, kDebounce * 2);
  ASSERT_EQ(r.a().state(), AlarmState::Alarm);

  r.a().requestClear();
  r.sample(true);
  EXPECT_EQ(r.a().state(), AlarmState::Clearing);
  EXPECT_TRUE(r.a().enaAsserted());
  EXPECT_TRUE(r.a().active());  // still inhibited: the drivers are OFF

  // Most of the way through the pulse, still high.
  r.hold(false, kPulse - (2 * Rig::kPeriodMs));
  EXPECT_TRUE(r.a().enaAsserted());

  // Past it, and ENA drops - but the state is still Clearing while it settles.
  r.hold(false, 2 * Rig::kPeriodMs);
  EXPECT_FALSE(r.a().enaAsserted());
  EXPECT_EQ(r.a().state(), AlarmState::Clearing);

  // Settle expires with the fault gone: cleared.
  r.hold(false, kSettle + Rig::kPeriodMs);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().active());
  EXPECT_FALSE(r.a().enaAsserted());
  EXPECT_FALSE(r.a().clearFailed());
}

TEST(AlarmMonitor, AClearAgainstAFaultThatIsStillPresentFailsVisibly) {
  Rig r;
  r.hold(true, kDebounce * 2);
  // Consumed here, as src/alarm.cpp consumes it: the point of the assertion at
  // the end is that the FAILED CLEAR raises no second edge, which only means
  // anything once the first one has been taken.
  ASSERT_TRUE(r.a().consumeTrip());
  r.a().requestClear();
  // The fault holds throughout - a crashed carriage that has not been freed.
  r.hold(true, kPulse + kSettle + (2 * Rig::kPeriodMs));

  EXPECT_EQ(r.a().state(), AlarmState::Alarm);
  EXPECT_TRUE(r.a().clearFailed());
  EXPECT_TRUE(r.a().inputAsserted());
  EXPECT_FALSE(r.a().enaAsserted());
  // A failed clear is not a new fault: nothing re-stops an axis that never
  // restarted, and the trip count still describes one event.
  EXPECT_FALSE(r.a().consumeTrip());
  EXPECT_EQ(r.a().trips(), 1u);
}

TEST(AlarmMonitor, ASecondClearAfterAFailedOneStillWorks) {
  Rig r;
  r.hold(true, kDebounce * 2);
  r.a().requestClear();
  r.hold(true, kPulse + kSettle + (2 * Rig::kPeriodMs));
  ASSERT_TRUE(r.a().clearFailed());

  // The operator frees the crash, then presses OK again.
  r.a().requestClear();
  r.sample(true);
  EXPECT_EQ(r.a().state(), AlarmState::Clearing);
  EXPECT_FALSE(r.a().clearFailed());  // dropped by the new attempt
  r.hold(false, kPulse + kSettle + (2 * Rig::kPeriodMs));
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().clearFailed());
}

// The input is not to be believed while the driver is held in reset: it is
// disabled and its alarm output says nothing about the fault. A line that reads
// FAULTED for the whole pulse and healthy the instant it ends must still clear.
TEST(AlarmMonitor, TheInputIsIgnoredForTheDurationOfThePulse) {
  Rig r;
  r.hold(true, kDebounce * 2);
  r.a().requestClear();
  r.sample(true);
  r.hold(true, kPulse);                           // asserted through the reset
  r.hold(false, kSettle + (2 * Rig::kPeriodMs));  // healthy once it is over
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().clearFailed());
}

TEST(AlarmMonitor, ASecondClearRequestDuringAPulseIsDroppedNotQueued) {
  Rig r;
  r.hold(true, kDebounce * 2);
  r.a().requestClear();
  r.sample(true);
  ASSERT_EQ(r.a().state(), AlarmState::Clearing);

  // An impatient second press, mid-pulse.
  r.a().requestClear();
  r.hold(false, kPulse + kSettle + (2 * Rig::kPeriodMs));
  ASSERT_EQ(r.a().state(), AlarmState::Clear);

  // If it had been queued, a second reset would fire here with nothing on
  // screen to account for it.
  r.hold(false, kPulse * 2);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().enaAsserted());
}

TEST(AlarmMonitor, AClearRequestWithNothingLatchedDoesNothing) {
  Rig r;
  r.a().requestClear();
  r.hold(false, kPulse * 2);
  EXPECT_EQ(r.a().state(), AlarmState::Clear);
  EXPECT_FALSE(r.a().enaAsserted());
}

// --- A second fault after a good clear -------------------------------------

TEST(AlarmMonitor, AFaultAfterAGoodClearTripsAgainAndIsCounted) {
  Rig r;
  r.hold(true, kDebounce * 2);
  r.a().requestClear();
  r.hold(false, kPulse + kSettle + (2 * Rig::kPeriodMs));
  ASSERT_EQ(r.a().state(), AlarmState::Clear);
  ASSERT_TRUE(r.a().consumeTrip());

  r.hold(true, kDebounce * 2);
  EXPECT_EQ(r.a().state(), AlarmState::Alarm);
  EXPECT_TRUE(r.a().consumeTrip());
  EXPECT_EQ(r.a().trips(), 2u);
}

// --- The invariant that matters most ---------------------------------------
//
// ENA high means the drivers are OFF. If any path out of the state machine
// leaves it high, the lathe is dead and nothing on screen says why. Walk a long
// mixed sequence of faults, clears and failed clears and assert the one-way
// implication that matters: ENA is high ONLY inside a clear attempt.
//
// Only one way round, deliberately. Clearing does NOT imply ENA high - the
// settle window is part of the clear and runs with the line already back down,
// which is exactly what makes the driver's answer readable.
TEST(AlarmMonitor, EnaIsNeverHighOutsideAClearAttempt) {
  Rig r;
  const bool pattern[] = {false, true, true, false, true, false, false, true};
  for (int round = 0; round < 8; round++) {
    for (int i = 0; i < 8; i++) {
      r.hold(pattern[i], kDebounce * 2);
      if (r.a().enaAsserted()) {
        EXPECT_EQ(r.a().state(), AlarmState::Clearing);
      }
    }
    r.a().requestClear();
    for (unsigned long t = 0; t < kPulse + kSettle + 100; t += Rig::kPeriodMs) {
      r.sample(pattern[round % 8]);
      if (r.a().enaAsserted()) {
        EXPECT_EQ(r.a().state(), AlarmState::Clearing);
      }
    }
    // ...and whatever that round did, the line is back down by the end of it.
    EXPECT_FALSE(r.a().enaAsserted());
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
