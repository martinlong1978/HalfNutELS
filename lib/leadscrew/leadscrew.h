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
  LeadscrewStopState getStopPositionState(LeadscrewStopPosition position);
  void unsetStopPosition(LeadscrewStopPosition position);
  int getStopPosition(LeadscrewStopPosition position);
  void setTargetPitchMM(float ratio);
  void setCurrentPosition(int position);
  void update();
  float getPositionError();
  LeadscrewDirection getCurrentDirection();
  float getEstimatedVelocityInMillimetersPerSecond();

  // TODO: implemented in FS-B
  // Carriage position in millimetres, converted from getCurrentPosition()
  // (pulses) via config->leadscrewStepsPerMm(). Stub returns a deliberately
  // wrong sentinel so callers relying on it fail loudly on assertion rather
  // than link error.
  float getPositionMM() { return -12345.0f; }

  // TODO: implemented in FS-B
  // Stop position in millimetres. ONLY valid when
  // getStopPositionState(position) == LeadscrewStopState::SET. When UNSET,
  // contract is to return NAN (test the result with std::isnan) rather than
  // converting the INT32_MIN/INT32_MAX pulse sentinels, which are meaningless
  // once divided by steps-per-mm.
  float getStopPositionMM(LeadscrewStopPosition position) { return -12345.0f; }

  // TODO: implemented in FS-B
  // Exposes the derived config so the display can format units without a
  // separate global. Stub returns nullptr so callers relying on it fail on
  // assertion (non-null check) rather than link error.
  LatheConfigDerived* getConfig() { return nullptr; }

};
