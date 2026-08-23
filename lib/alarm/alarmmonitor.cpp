#include "alarmmonitor.h"

// Definitions for the in-class constants. Needed because the host tests take
// their address (gtest compares by const reference), which is an ODR use.
const unsigned long AlarmMonitor::kDebounceMs;
const unsigned long AlarmMonitor::kEnaPulseMs;
const unsigned long AlarmMonitor::kSettleMs;

AlarmMonitor::AlarmMonitor()
    // EVERY member, explicitly. This object is a file-scope global today
    // (src/alarm.cpp), but the pattern in this repo is that anything reachable
    // from a `new` in main.cpp must not rely on zeroing - see CLAUDE.md
    // ("Constructors must initialise all members"), which has cost real bugs
    // here. m_debounced starting false is also load-bearing: it is what makes
    // a device that boots with the line already low take one full kDebounceMs
    // to latch, rather than tripping on whatever the pin read before the
    // driver's own supply had come up.
    : m_raw(false),
      m_debounced(false),
      m_rawSinceMs(0),
      m_state(AlarmState::Clear),
      m_enaAsserted(false),
      m_pulseStartMs(0),
      m_settleStartMs(0),
      m_settling(false),
      m_clearRequested(false),
      m_tripPending(false),
      m_clearFailed(false),
      m_trips(0) {}

bool AlarmMonitor::consumeTrip() {
  const bool out = m_tripPending;
  m_tripPending = false;
  return out;
}

void AlarmMonitor::update(bool alarmAsserted, unsigned long nowMs) {
  // --- 1. The input filter -------------------------------------------------
  //
  // Frozen for the whole of a clear attempt. During the ENA pulse the driver is
  // disabled and its alarm output says nothing meaningful about the fault, and
  // during the settle it is coming back up; believing either would decide the
  // outcome of the clear from the state of the machine mid-reset. The filter is
  // re-seeded from a fresh reading when the settle expires, below.
  if (m_state != AlarmState::Clearing) {
    if (alarmAsserted != m_raw) {
      m_raw = alarmAsserted;
      m_rawSinceMs = nowMs;
    } else if (m_raw != m_debounced && (nowMs - m_rawSinceMs) >= kDebounceMs) {
      m_debounced = m_raw;
    }
  }

  // --- 2. The state machine ------------------------------------------------
  switch (m_state) {
    case AlarmState::Clear:
      if (m_debounced) {
        m_state = AlarmState::Alarm;
        m_tripPending = true;
        m_trips++;
      }
      break;

    case AlarmState::Alarm:
      // The latch does NOT release when the input does - see the class comment.
      // The only way out is an acknowledged clear.
      if (m_clearRequested) {
        m_clearRequested = false;
        m_clearFailed = false;
        m_state = AlarmState::Clearing;
        m_enaAsserted = true;
        m_pulseStartMs = nowMs;
        m_settling = false;
      }
      break;

    case AlarmState::Clearing:
      // A clear request that arrives during a pulse is dropped, not queued:
      // firing a second reset at some later moment, with no dialog on screen
      // to account for it, is worse than ignoring an impatient second press.
      m_clearRequested = false;
      if (!m_settling) {
        if ((nowMs - m_pulseStartMs) >= kEnaPulseMs) {
          m_enaAsserted = false;
          m_settling = true;
          m_settleStartMs = nowMs;
        }
      } else if ((nowMs - m_settleStartMs) >= kSettleMs) {
        // Re-seed the filter from the level the line holds NOW, and judge the
        // clear on it directly rather than waiting another kDebounceMs: the
        // settle window is already six debounce periods long, so this reading
        // has had far longer to steady than the filter would ever demand.
        m_raw = alarmAsserted;
        m_debounced = alarmAsserted;
        m_rawSinceMs = nowMs;
        m_settling = false;
        if (alarmAsserted) {
          // Still faulted. Back to Alarm, and NO new trip: the machine never
          // left the alarm, so the once-per-trip work has already been done and
          // repeating it would stop an axis that is already stopped and count a
          // fault that never re-occurred.
          m_state = AlarmState::Alarm;
          m_clearFailed = true;
        } else {
          m_state = AlarmState::Clear;
          m_clearFailed = false;
        }
      }
      break;
  }

  // --- 3. ENA rests LOW ----------------------------------------------------
  // Stated unconditionally rather than per-branch, so no path through the
  // machine can leave the drivers disabled behind it. The pulse is the single
  // exception and it is the one state tested for here.
  if (m_state != AlarmState::Clearing) {
    m_enaAsserted = false;
  }
}
