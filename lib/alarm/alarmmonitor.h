// The stepper driver's ALARM input and the ENABLE line that clears it.
//
// Pure C++ on purpose: NO Arduino / ESP / FreeRTOS includes, so it builds and is
// unit-tested on the native host (`pio test -e native`), exactly like
// lib/keyscan and lib/ui. It reads no pin and writes no pin - src/alarm.cpp does
// both, and this class decides everything in between.
//
// WHAT IT MODELS
//
// The driver has an open-collector alarm output (IO27, active LOW - see the
// polarity note in lib/config/board.h) and an enable input (IO17, LOW = enabled).
// A latched driver fault is cleared by pulsing ENA HIGH and back LOW, which is
// what the panel switch SW1 does by hand. So there are exactly three states:
//
//   Clear     nothing wrong. ENA low.
//   Alarm     a fault has been seen and LATCHED. ENA low, motion inhibited.
//   Clearing  the operator pressed OK; ENA is being held HIGH for kEnaPulseMs
//             and then dropped, after which the input is re-read.
//
// THE LATCH IS THE POINT. The alarm does not go away by itself when the input
// de-asserts, and that is deliberate rather than lazy: a fault that trips and
// releases still means the driver stopped stepping, so the carriage is no
// longer where the software thinks it is and the thread sync is worthless. The
// operator has to be told, and the acknowledgement is what tells us they were.
//
// WHAT IT DOES NOT DO. It never stops the machine and it never touches
// GlobalState - it is a decision function. src/alarm.cpp applies the two
// outputs (enaAsserted(), state()) to the pin and to the motion mode.
#ifndef ELS_ALARM_ALARMMONITOR_H
#define ELS_ALARM_ALARMMONITOR_H

enum class AlarmState {
  Clear,     // no fault
  Alarm,     // latched; the modal is up and the machine is inhibited
  Clearing,  // ENA pulse in flight, or settling after it
};

class AlarmMonitor {
 public:
  AlarmMonitor();

  // One sample of the alarm INPUT, already reduced to "is the driver asserting
  // a fault" (i.e. the pin read compared against ELS_STEPPER_ALARM_ACTIVE_LEVEL
  // by the caller - this class knows nothing about pin levels).
  //
  // Call at a steady period; kDebounceMs is wall-clock, not a sample count, so
  // the period may change without changing the filter.
  void update(bool alarmAsserted, unsigned long nowMs);

  AlarmState state() const { return m_state; }

  // True while the machine must be held stopped and the modal shown - i.e.
  // anything but Clear. The clear pulse is included on purpose: the drivers are
  // DISABLED during it, so that is the last moment to let motion start.
  bool active() const { return m_state != AlarmState::Clear; }

  // What the ENA line should be right now. True = drive it HIGH (drivers
  // disabled / fault reset), false = drive it LOW (the resting state).
  bool enaAsserted() const { return m_enaAsserted; }

  // The operator pressed OK on the alarm modal. Cross-task: set on the
  // DisplayTask (via ButtonPad), consumed by update() on the alarm task, so it
  // is volatile and written by exactly one task and cleared by exactly one
  // other - the same single-producer/single-consumer bargain KeyArray's ring
  // buffer makes (CLAUDE.md, cross-task state: no locks).
  //
  // Ignored unless the state is Alarm: a request that arrives while a pulse is
  // already in flight, or when there is nothing to clear, is dropped rather
  // than queued. Queueing it would fire a second pulse at some arbitrary later
  // moment with no dialog on screen to explain it.
  void requestClear() { m_clearRequested = true; }

  // Rising edge of the latch, consumed once. The caller uses it to do the
  // things that must happen exactly ONCE per trip (stop the axis, drop the
  // sync state); everything that must hold for the DURATION of the alarm is
  // driven off active() instead, so a missed edge cannot leave the machine
  // live. Returns true at most once per trip.
  bool consumeTrip();

  // The last clear attempt finished with the input still asserted, i.e. the
  // fault is still present at the driver. Reset by the next clear request and
  // by a successful clear. The modal reads it to say so rather than looking
  // like OK did nothing.
  bool clearFailed() const { return m_clearFailed; }

  // The DEBOUNCED input, i.e. whether the driver is asserting a fault right
  // now, independent of the latch. The modal reads it to tell the operator
  // whether there is still something to fix ("fault present at the driver")
  // or whether the machine is merely waiting to be acknowledged.
  //
  // Stale during Clearing, deliberately: the filter is frozen for the whole
  // clear attempt (see update()), so this holds the pre-pulse reading until
  // the settle re-seeds it. Nothing shows it in that state - the modal reads
  // the state word instead.
  bool inputAsserted() const { return m_debounced; }

  // Total trips since boot. Diagnostics only - a machine that alarms once a
  // week and one that alarms twice an hour are different machines, and that is
  // not visible from a modal that has been dismissed.
  unsigned long trips() const { return m_trips; }

  // A reading must persist this long before it is believed, in either
  // direction. The line runs the length of the loom to the driver, so a
  // conducted spike must not put a modal on the screen mid-cut - and equally,
  // a fault that flickers must not be filtered away into nothing.
  //
  // 25 ms rather than lib/keyscan's 8: this is not a bouncing contact, it is a
  // steady level on a long wire beside the step and direction pairs, and there
  // is nothing to be gained by reacting faster than the operator can look up.
  static const unsigned long kDebounceMs = 25;

  // How long ENA is held HIGH to reset the driver. The owner's figure; also
  // comfortably longer than any driver's own reset window, and short enough
  // that the modal's "clearing" state does not read as a hang.
  static const unsigned long kEnaPulseMs = 1000;

  // Quiet period after ENA returns LOW, before the input is believed again.
  // The driver needs a moment to re-enable and release its alarm output, and
  // the debounce filter above is only restarted once this expires - so a clear
  // is never judged against the level the line held DURING its own reset.
  static const unsigned long kSettleMs = 150;

 private:
  // The debounced view of the input. m_raw is the last raw sample, m_rawSinceMs
  // when it first read that way, and m_debounced the value that has actually
  // survived kDebounceMs.
  bool m_raw;
  bool m_debounced;
  unsigned long m_rawSinceMs;

  AlarmState m_state;
  bool m_enaAsserted;
  unsigned long m_pulseStartMs;   // when ENA went HIGH
  unsigned long m_settleStartMs;  // when ENA came back LOW; 0 while pulsing
  bool m_settling;

  volatile bool m_clearRequested;
  bool m_tripPending;
  bool m_clearFailed;
  unsigned long m_trips;
};

#endif
