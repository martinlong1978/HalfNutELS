#include <spindle.h>
#include <axis.h>
#include <Arduino.h>
#include "leadscrew_io.h"
#include "leadscrew_stopsync.h"
#include "globalstate.h"
#include "latheconfig.h"
#pragma once


// only run for unit tests
#if PIO_UNIT_TESTING
#undef ELS_INVERT_DIRECTION
#endif

/**
 * The current direction of the leadscrew
 * We set numbers to use later when actually moving the position
 */
enum class LeadscrewDirection { LEFT = -1, RIGHT = 1, UNKNOWN = 0 };


class Leadscrew : public LinearAxis, public DerivedAxis, public DrivenAxis {
private:

#ifdef ELS_USE_RMT
  rmt_data_t rmt_data[24];
  rmt_obj_t *rmtObj;
#endif

  LatheConfigDerived *config;

  Spindle* m_spindle;
  LeadscrewIO* m_io;

  float m_expectedPosition;

  // the ratio of how much the leadscrew moves per spindle rotation
  const int motorPulsePerRevolution;
  const float leadscrewPitch;
  // the number of pulses per revolution of the lead axis (spindle)
  const int encoderPPR;
  float m_ratio;

  // The current delay between pulses in microseconds
  const float initialPulseDelay;
  float m_currentPulseDelay;
  float m_leadscrewSpeed;
  const float m_leadscrewAccel;
  LeadscrewDirection m_currentDirection;

  // Cold stop-position + spindle-sync state (button-event driven); update()
  // still reads its plain fields / trivial inline predicates directly.
  LeadscrewStopSync m_stopSync;

  /**
   * This gets the "unit" of the accumulator, i.e the amount the accumulator
   * increased by when the leadscrew position increases by 1
   */
  bool sendPulse();
  int getStoppingDistanceInPulses();
  int getTargetSpeedDistanceInPulses();
  uint64_t jogMicros;

  int debugPulseCount;
  bool initPos;

  GlobalMotionMode m_motionMode = MM_DISABLED;
  GlobalState *m_globalState;

public:
  Leadscrew(LatheConfigDerived *config, Spindle* spindle, LeadscrewIO* io,
    float leadscrewAccel, float initialPulseDelay, 
    int motorPulsePerRevolution,
    float leadscrewPitch, int encoderPPR);
  #ifdef ELS_USE_RMT
  void setRMT(rmt_obj_t *rmtObj){
    this->rmtObj = rmtObj;
    rmt_data->duration0 = 8;
    rmt_data->level0 = 1;
    rmt_data->duration1 = 8;
    rmt_data->level1 = 0;
  
  }
  #endif


  void setStopPosition(LeadscrewStopPosition position);
  void setStopPosition(LeadscrewStopPosition position, int stopPosition);

  /**
   * Anchor the thread helix to the CURRENT spindle angle and the CURRENT
   * carriage position ("pick up an existing thread", docs/ux-redesign.md Sec. 6).
   *
   * The gesture: the spindle is stopped, the user hand-positions the tool so it
   * sits in an existing groove, then declares "this spindle angle and this
   * carriage position are in sync". Every later engagement re-enters that same
   * helix instead of ploughing a new groove across the existing one.
   *
   * Contract (pinned by test/test_sync_point):
   *  - takes BOTH coordinates itself, at one instant, from m_currentPosition and
   *    m_spindle->getCurrentPosition(). It deliberately has no parameters: it is
   *    called from the DisplayTask while the SpindleTask is inside update(), so
   *    the caller must not be able to sample the two coordinates separately (or
   *    at a different time) and hand in a skewed pair.
   *  - sets GlobalThreadSyncState to SS_SYNC (at the instant of the call the
   *    carriage is on the helix by definition).
   *  - works with NO stops set, and must not create, move or clear either stop.
   *  - the anchor it records is its OWN (carriage, spindle-phase) pair, not a
   *    stop-derived one, so it must survive setStop()/unsetStop() untouched -
   *    unsetStop()'s re-anchor migration exists only because a stop-derived
   *    anchor loses its carriage coordinate when the stop goes away, which does
   *    not apply here. An explicit user sync outranks an incidental one.
   *  - the last call wins.
   *
   * Implemented as a LeadscrewSpindleSyncPositionState::MANUAL anchor, which
   * carries its own carriage coordinate (LeadscrewStopSync::manualSyncPosition)
   * rather than borrowing a stop's. That is what makes it independent of the
   * stop machinery: setStop()'s "latch only when the anchor is UNSET" guard and
   * unsetStop()'s "only re-anchor a LEFT/RIGHT anchor" guards both leave a
   * MANUAL anchor alone without any extra code.
   *
   * Cold path (menu / button event only): defined out-of-line in leadscrew.cpp
   * next to setStopPosition(). It adds nothing to Leadscrew::update() beyond one
   * more arm on the switch that already selects the anchor's carriage position.
   */
  void setSyncPoint();
  LeadscrewStopState getStopPositionState(LeadscrewStopPosition position);
  void unsetStopPosition(LeadscrewStopPosition position);
  int getStopPosition(LeadscrewStopPosition position);
  void setTargetPitchMM(float ratio);
  void setCurrentPosition(int position);
  void update();
  float getPositionError();
  LeadscrewDirection getCurrentDirection();
  float getEstimatedVelocityInMillimetersPerSecond();

  // Carriage position in millimetres, converted from getCurrentPosition()
  // (pulses) via config->leadscrewStepsPerMm(). Display-path only (not called
  // from update()); cheap divide by an already-cached derived value.
  float getPositionMM() { return getCurrentPosition() / config->leadscrewStepsPerMm(); }

  // Stop position in millimetres. ONLY valid when
  // getStopPositionState(position) == LeadscrewStopState::SET. When UNSET,
  // returns NAN rather than converting the INT32_MIN/INT32_MAX pulse
  // sentinels, which are meaningless once divided by steps-per-mm.
  float getStopPositionMM(LeadscrewStopPosition position) {
    if (getStopPositionState(position) != LeadscrewStopState::SET) {
      return NAN;
    }
    return getStopPosition(position) / config->leadscrewStepsPerMm();
  }

  // Exposes the derived config so the display can format units without a
  // separate global. const-qualified return: config is fixed at runtime
  // (see the LatheConfigDerived class comment in latheconfig.h) and callers
  // outside Leadscrew have no business mutating it through this pointer.
  const LatheConfigDerived* getConfig() { return config; }

};
