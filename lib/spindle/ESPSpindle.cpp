#include <config.h>
#include "Spindle.h"
#include "latheconfig.h"

#include <math.h>

Spindle::Spindle(int pinA, int pinB, LatheConfigDerived *latheConfig) : m_encoder(), config(latheConfig) {
  ESP32Encoder::useInternalWeakPullResistors = puType::none;
  m_encoder.attachFullQuad(pinA, pinB);
  gpio_pullup_en((gpio_num_t)pinA);
  gpio_pullup_en((gpio_num_t)pinB);
  

  m_unconsumedPosition = 0;
  m_lastPulseTimestamp = micros();
  m_lastFullPulseDurationMicros = 0;
  m_currentPosition = 0;
}

#ifdef ELS_OFFLINE
#define TEST_SPEED_DIVISOR 15

void Spindle::update() {
  // read the encoder and update the current position
  // todo: we should keep the absolute position of the spindle, cbf right now
  unsigned long mic = micros();
  int amount = -(((int)(mic - m_lastFetchTime)) / TEST_SPEED_DIVISOR);
  incrementCurrentPosition(amount);
  m_lastFetchTime += (amount * -(TEST_SPEED_DIVISOR));
}
#else

// The largest spindle delta one update() can legitimately see. ESP32Encoder
// runs the PCNT counter to +-INT16 (counter_h_lim/_l_lim = _INT16_MAX/MIN) and
// accumulates the wrap in a limit ISR; a getAndClearCount() that races that ISR
// returns a spurious value at the 16-bit boundary.
//
// MEASURED on the lathe, mid-cut (tools/debugsink capture 20260818-230340):
// two excursions, both identical - one sample of delta +32765, the next
// -32766, nearly cancelling. Leadscrew::update() believed them, multiplied by
// the ratio into m_expectedPosition and saturated posError to -2^31. That is
// the large forward jump then correction reported while threading, and the
// audible jitter is the same race at smaller amplitude. The loop gap stayed at
// 30-58 us throughout, so this was never CPU starvation.
//
// 16384 is half the 16-bit range: far above anything physical, far below the
// glitch. A real delta is well under one count per iteration (1200 PPR at
// 3000 rpm is 60 counts/ms against a loop running at ~70 kHz), and even a
// 100 ms hiccup at that speed is only ~6000. Rejecting is correct rather than
// merely safe: the reading is not motion that happened, so dropping it keeps
// the spindle position TRUE and leaves thread sync intact.
static const int64_t kMaxPlausibleSpindleDelta = 16384;

void Spindle::update() {
  // read the encoder and update the current position
  // todo: we should keep the absolute position of the spindle, cbf right now
  int64_t position = m_encoder.getAndClearCount();
  if (position > kMaxPlausibleSpindleDelta ||
      position < -kMaxPlausibleSpindleDelta) {
    // Deliberately dropped, not clamped: clamping would inject 16384 counts of
    // motion that never happened. The counts are already gone from the
    // encoder (getAndClearCount consumed them), which is what makes the pair
    // self-cancelling - the true position is unchanged.
    return;
  }
  incrementCurrentPosition(position);
}
#endif

void Spindle::setCurrentPosition(int position) {
  // update the unconsumed position by finding the delta between the old and new
  // positions
  int positionDelta = position - m_currentPosition;
  m_unconsumedPosition += positionDelta;

  int ppr = config->spindleEncoderPpr();
  
  m_currentPosition = (position + ppr) % ppr;
}

void Spindle::incrementCurrentPosition(int amount) {
  int64_t t = micros();
  int pos = getCurrentPosition() + amount;
  setCurrentPosition(pos);
  int newpos = getCurrentPosition();
  if (pos != newpos) // spindle pos has wrapped
  {
    m_lastRevPosition -= pos - newpos;
  }
  if (amount != 0) {
    m_lastPulseTimestamp = t;
    if (abs(newpos - m_lastRevPosition) > ELS_SPEED_COUNTS) {
      // Update stats for last full revolution. 
      m_lastRevSize = newpos - m_lastRevPosition;
      m_lastRevPosition = newpos;
      m_lastRevMicros = t - m_lastRevTimestamp;
      m_lastRevTimestamp = t;
    }
  }
}

float Spindle::getEstimatedVelocityInPPS() {
  if (m_lastRevMicros == 0)return 0;
  if(micros() - m_lastRevTimestamp > 1000000)return 0;
  return (m_lastRevSize * US_PER_SECOND) / (m_lastRevMicros);
}


float Spindle::getEstimatedVelocityInRPM() {
  if (m_lastRevMicros == 0)return 0;
  if (micros() - m_lastRevTimestamp > 1000000)return 0;
  return -((m_lastRevSize * 60000000) / (m_lastRevMicros * config->spindleEncoderPpr()));
}

int Spindle::consumePosition() {
  int position = m_unconsumedPosition;
  m_unconsumedPosition = 0;
  return position;
}
