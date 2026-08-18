#ifndef PIO_UNIT_TESTING
#include <Wire.h>
#endif
#include <Arduino.h>  // micros(), for seeding the capture's relative clock
#include <globalstate.h>

GlobalState* GlobalState::m_instance = nullptr;
GlobalState* GlobalState::getInstance() {
  if (m_instance == nullptr) {
    m_instance = new GlobalState();
  }
  return m_instance;
}

bool GlobalState::getDebugMode() {
  return m_debug.recording();
}

// Arm or discard a motion-trace capture.
//
// This used to be an empty function body with the original commented out above
// it. The original malloc'd 100 KB on enable and, on disable, dumped the whole
// trace over Serial at 921600 baud FROM WHEREVER IT WAS CALLED - which was
// inside Leadscrew::update(), on the 4 KB SpindleTask, once the buffer filled.
// Both halves of that are gone: the buffer is owned by DebugCapture (so the
// enable flag cannot outlive the allocation), and the trace leaves the machine
// over HTTP from its own task, only once the carriage is at rest
// (src/DebugSink.cpp).
//
// Cold path. Called from the DisplayTask when the "Debug capture" menu tile is
// activated, which menuTileBlock() refuses while the carriage is under power -
// arm() calls malloc, and a ~100 KB allocation mid-cut is exactly the kind of
// thing that stalls a hot loop.
void GlobalState::setDebugMode(bool mode) {
  if (mode) {
    m_debug.arm((uint32_t)micros());
  } else {
    m_debug.discard();
  }
}


void GlobalState::IncFeedMode() {
  switch (m_feedMode) {
  case FM_UNSET:
  case FM_JOG:
    // FM_JOG is no longer produced by this cycle (docs/ux-redesign.md Sec.
    // 3: "FM_JOG disappears from the mode cycle"); this branch only exists
    // as a defensive fallback for FM_UNSET/leftover FM_JOG state.
    m_feedMode = FM_FEED;
    break;
  case FM_FEED:
    m_feedMode = FM_THREAD;
    break;
  case FM_THREAD:
    m_feedMode = FM_THREAD_REVERSE;
    break;
  case FM_THREAD_REVERSE:
    m_feedMode = FM_FEED;
    break;
  }

  // Restore the remembered pitch index for this (unit, mode-type) slot
  // instead of resetting to the mode/unit default every time.
  setFeedSelect(
      m_pitchMemory[pitchMemoryUnitIndex(m_unitMode)][pitchMemoryTypeIndex(m_feedMode)]);
}

GlobalFeedMode GlobalState::getFeedMode() { return m_feedMode; }

float GlobalState::getJogSpeed() {
  return jogSpeeds[m_jogSpeed];
}

int GlobalState::getJogIndex() {
  return m_jogSpeed;
}

void GlobalState::incJogSpeed() {
  m_jogSpeed = min((int)ARRAY_SIZE(jogSpeeds) - 1, m_jogSpeed + 1);
}
void GlobalState::decJogSpeed() {
  m_jogSpeed = max(0, m_jogSpeed - 1);
}

int GlobalState::getFeedSelect() { return m_feedSelect; }
int GlobalState::getCurrentFeedSelectArraySize() {
  // this just ensures that the feedSelect doesn't go out of bounds for the
  // current arry
  if (m_unitMode == METRIC) {
    if (m_feedMode == FM_THREAD || m_feedMode == FM_THREAD_REVERSE) {
      return ARRAY_SIZE(threadPitchMetric);
    } else {
      return ARRAY_SIZE(feedPitchMetric);
    }
  } else {
    if (m_feedMode == FM_THREAD || m_feedMode == FM_THREAD_REVERSE) {
      return ARRAY_SIZE(threadPitchImperial);
    } else {
      return ARRAY_SIZE(feedPitchImperial);
    }
  }

  // invalid - should never get here!
  return -1;
}

void GlobalState::setFeedSelect(int select) {
  if (select >= 0 && select < getCurrentFeedSelectArraySize()) {
    m_feedSelect = select;
  } else {
    // if we're out of bounds, just set the default
    if (m_feedMode == FM_THREAD || m_feedMode == FM_THREAD_REVERSE) {
      if (m_unitMode == METRIC) {
        m_feedSelect = DEFAULT_METRIC_THREAD_PITCH_IDX;
      } else {
        m_feedSelect = DEFAULT_IMPERIAL_THREAD_PITCH_IDX;
      }
    } else {
      if (m_unitMode == METRIC) {
        m_feedSelect = DEFAULT_METRIC_FEED_PITCH_IDX;
      } else {
        m_feedSelect = DEFAULT_IMPERIAL_FEED_PITCH_IDX;
      }
    }
  }

  // Keep this (unit, mode-type) slot's remembered index in sync so a later
  // mode/unit switch back to it restores what was just set here.
  m_pitchMemory[pitchMemoryUnitIndex(m_unitMode)][pitchMemoryTypeIndex(m_feedMode)] =
      m_feedSelect;
}

float GlobalState::getCurrentFeedPitch() {
  bool thread = (m_feedMode == FM_THREAD || m_feedMode == FM_THREAD_REVERSE);
  float pitch;
  if (m_unitMode == METRIC) {
    pitch = thread ? threadPitchMetric[m_feedSelect] : feedPitchMetric[m_feedSelect];
  } else if (thread) {
    // threads are defined in TPI, not pitch
    pitch = (1.0 / threadPitchImperial[m_feedSelect]) * 25.4;
  } else {
    // feeds are defined in thou/rev, not mm/rev
    pitch = feedPitchImperial[m_feedSelect] * 25.4 / 1000;
  }
  // Reverse thread runs the same pitch but drives the leadscrew the opposite
  // way (from the left stop toward the right). A negative pitch negates the
  // leadscrew:spindle ratio in Leadscrew::setTargetPitchMM.
  if (m_feedMode == FM_THREAD_REVERSE) {
    pitch = -pitch;
  }
  return pitch;
}

int GlobalState::nextFeedPitch() {
  if (m_feedSelect != getCurrentFeedSelectArraySize() - 1) {
    setFeedSelect(m_feedSelect + 1);
  }

  return m_feedSelect;
}

int GlobalState::prevFeedPitch() {
  if (m_feedSelect != 0) {
    setFeedSelect(m_feedSelect - 1);
  }

  return m_feedSelect;
}

void GlobalState::setMotionMode(GlobalMotionMode mode) { m_motionMode = mode; }

GlobalMotionMode GlobalState::getMotionMode() { return m_motionMode; }

void GlobalState::setUnitMode(GlobalUnitMode mode) {
  m_unitMode = mode;
  // Restore the remembered pitch index for the newly-selected unit's slot
  // (same mode-type), bounds-checked by setFeedSelect() against the array
  // for the new unit - keeps getFeedSelect()/getCurrentFeedPitch() sane
  // regardless of pitch-table sizes.
  setFeedSelect(
      m_pitchMemory[pitchMemoryUnitIndex(m_unitMode)][pitchMemoryTypeIndex(m_feedMode)]);
}

GlobalUnitMode GlobalState::getUnitMode() { return m_unitMode; }

void GlobalState::setThreadSyncState(GlobalThreadSyncState state) {
  m_threadSyncState = state;
}

GlobalThreadSyncState GlobalState::getThreadSyncState() {
  return m_threadSyncState;
}

bool  GlobalState::hasOTA() { return OTA; };
void GlobalState::setOTA() { OTA = true; };
void  GlobalState::clearOTA() { OTA = false; };

void  GlobalState::setOTABytes(int bytes) { OTAbytes = bytes; }
int  GlobalState::getOTABytes() { return OTAbytes; }
int  GlobalState::getOTALength() { return OTAlength; }
void  GlobalState::setOTAContentLength(int length) { OTAlength = length; }

void  GlobalState::setOtaStatus(GlobalOtaStatus status) { m_otaStatus = status; }
GlobalOtaStatus  GlobalState::getOtaStatus() { return m_otaStatus; }


void  GlobalState::setDisplayReset() { m_displayReset = true; }
bool  GlobalState::getDisplayReset() {
  bool ret = m_displayReset;
  m_displayReset = false;
  return ret;
}
