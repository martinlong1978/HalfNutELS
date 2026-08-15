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
  setCurrentPosition(getCurrentPosition() + amount);
}

float Spindle::getEstimatedVelocityInRPM() { return 0.0f; }

float Spindle::getEstimatedVelocityInPPS() { return 0.0f; }

int Spindle::consumePosition() {
  int position = m_unconsumedPosition;
  m_unconsumedPosition = 0;
  return position;
}
#endif
