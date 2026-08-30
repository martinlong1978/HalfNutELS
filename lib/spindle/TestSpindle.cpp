#ifdef PIO_UNIT_TESTING
// Native (host) test double for Spindle.
//
// This is the ONLY Spindle:: definition compiled for the `native` env; the real
// hardware implementation (ESPSpindle.cpp) is excluded from that build (see
// scripts/exclude_espspindle_native.py). The behaviour here mirrors the real
// ESPSpindle position bookkeeping (modulo-PPR wrapping + unconsumed-delta
// accounting) so that Leadscrew characterization tests observe realistic
// spindle-driven motion, but with the encoder and velocity estimation stubbed
// out for determinism.
#include "spindle.h"

#include <config.h>
#include <math.h>

Spindle::Spindle(int pinA, int pinB, LatheConfigDerived *config)
    : m_encoder(), config(config) {
  (void)pinA;
  (void)pinB;
  m_unconsumedPosition = 0;
  m_lastFetchTime = 0;
  m_lastFullPulseDurationMicros = 0;
  m_currentPosition = 0;
}

void Spindle::update() {
  // No encoder on host; position is driven directly by tests via
  // setCurrentPosition()/incrementCurrentPosition().
}

void Spindle::setCurrentPosition(int position) {
  // Track the delta between the old and new positions as unconsumed motion,
  // then wrap the stored position into [0, ppr) exactly like ESPSpindle.
  int positionDelta = position - m_currentPosition;
  m_unconsumedPosition += positionDelta;

  int ppr = config->spindleEncoderPpr();
  m_currentPosition = positiveModulo(position, ppr);
}

void Spindle::incrementCurrentPosition(int amount) {
  // Mirror ESPSpindle::incrementCurrentPosition: keep the windowed
  // last-revolution bookkeeping (m_lastRev*) so getEstimatedVelocityInPPS()
  // reports a realistic, time-based velocity to the leadscrew feed-forward.
  // The Axis base members (m_lastRev*, m_lastPulseTimestamp) are zero
  // initialised, and micros() is the test virtual clock.
  int64_t t = micros();
  int pos = getCurrentPosition() + amount;
  setCurrentPosition(pos);
  int newpos = getCurrentPosition();
  if (pos != newpos) {  // spindle position has wrapped
    m_lastRevPosition -= pos - newpos;
  }
  if (amount != 0) {
    m_lastPulseTimestamp = t;
    int64_t diff = newpos - m_lastRevPosition;
    if (diff < 0) {
      diff = -diff;
    }
    if (diff > ELS_SPEED_COUNTS) {
      // Update stats for the last "full revolution" window.
      m_lastRevSize = newpos - m_lastRevPosition;
      m_lastRevPosition = newpos;
      m_lastRevMicros = t - m_lastRevTimestamp;
      m_lastRevTimestamp = t;
    }
  }
}

float Spindle::getEstimatedVelocityInRPM() {
  if (m_lastRevMicros == 0) return 0;
  if (micros() - m_lastRevTimestamp > 1000000) return 0;
  return -((m_lastRevSize * 60000000) /
           (m_lastRevMicros * config->spindleEncoderPpr()));
}

float Spindle::getEstimatedVelocityInPPS() {
  if (m_lastRevMicros == 0) return 0;
  if (micros() - m_lastRevTimestamp > 1000000) return 0;
  return (m_lastRevSize * US_PER_SECOND) / (m_lastRevMicros);
}

int Spindle::consumePosition() {
  int position = m_unconsumedPosition;
  m_unconsumedPosition = 0;
  return position;
}
#endif
