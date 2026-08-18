#include "leadscrew.h"

#include <globalstate.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "leadscrew_io.h"
using namespace std;

/**
 * TODO: This is kind of a god object, we should probably split this up into more manageable parts
 * I'm thinking that this class should be responsible for the position only.
 * Another class should handle the motor control and acceleration
 */

Leadscrew::Leadscrew(LatheConfigDerived *config, Spindle* spindle, LeadscrewIO* io,
  float leadscrewAccel, float initialPulseDelay,
  int motorPulsePerRevolution,
  float leadscrewPitch, int encoderPPR)
  : config(config), motorPulsePerRevolution(motorPulsePerRevolution),
  leadscrewPitch(leadscrewPitch),
  encoderPPR(encoderPPR),
  m_io(io),
  m_spindle(spindle),
  initPos(false),
  m_currentDirection(LeadscrewDirection::UNKNOWN),
  m_leadscrewAccel(leadscrewAccel),
  m_leadscrewSpeed(0),
  initialPulseDelay(initialPulseDelay),
  m_currentPulseDelay(initialPulseDelay) {
  m_globalState = GlobalState::getInstance();
  setTargetPitchMM(m_globalState->getCurrentFeedPitch());
  m_lastPulseTimestamp = micros();
  m_lastFullPulseDurationMicros = 0;
  m_expectedPosition = 0;
  m_currentPosition = 0;
  // Heap-allocated in main.cpp; the stop/sync state must not start from stale
  // memory or setStopPosition()'s "sync only when UNSET" guard behaves
  // non-deterministically. m_stopSync's default constructor initialises all of
  // its fields (stops UNSET, sync anchor UNSET).
  debugPulseCount = 0;
  jogMicros = 0;
}

void Leadscrew::setTargetPitchMM(float pitch) {
  // Calculate the ratio one, when the pitch is set. No need to calculate this every cycle
  m_ratio = (pitch * (float)motorPulsePerRevolution) / (leadscrewPitch * (float)encoderPPR);
}


/**
 * Cold stop/sync mutators, owned by LeadscrewStopSync (moved verbatim from the
 * old Leadscrew methods; only the member names lost their m_ prefix).
 */
void LeadscrewStopSync::unsetStop(LeadscrewStopPosition position, float ratio, int encoderPPR) {
  // Capture the pre-reset state: if this stop currently holds the spindle-sync
  // anchor and the OTHER stop is set, the anchor must MOVE to that stop. The
  // helix relation used by update() is
  //   spindlePhase(L) = positiveModulo((int)((L - syncStop)/ratio) + spindleSyncPosition, encoderPPR)
  // so re-anchoring from `source` (the stop being unset) to `dest` requires
  //   newSync = positiveModulo((int)((dest - source)/ratio) + oldSync, encoderPPR)
  // These reads must happen BEFORE the source position is reset to its sentinel.
  switch (position) {
  case LeadscrewStopPosition::LEFT:
    if (syncPositionState == LeadscrewSpindleSyncPositionState::LEFT) {
      if (rightStopState == LeadscrewStopState::SET) {
        // extrapolate the sync position to the other endstop
        spindleSyncPosition = positiveModulo(
            (int)((float)(rightStopPosition - leftStopPosition) / ratio) + spindleSyncPosition,
            encoderPPR);
        syncPositionState = LeadscrewSpindleSyncPositionState::RIGHT;
      } else {
        syncPositionState = LeadscrewSpindleSyncPositionState::UNSET;
      }
    }
    leftStopState = LeadscrewStopState::UNSET;
    leftStopPosition = INT32_MIN;
    break;
  case LeadscrewStopPosition::RIGHT:
    if (syncPositionState == LeadscrewSpindleSyncPositionState::RIGHT) {
      if (leftStopState == LeadscrewStopState::SET) {
        // extrapolate the sync position to the other endstop
        spindleSyncPosition = positiveModulo(
            (int)((float)(leftStopPosition - rightStopPosition) / ratio) + spindleSyncPosition,
            encoderPPR);
        syncPositionState = LeadscrewSpindleSyncPositionState::LEFT;
      } else {
        syncPositionState = LeadscrewSpindleSyncPositionState::UNSET;
      }
    }
    rightStopState = LeadscrewStopState::UNSET;
    rightStopPosition = INT32_MAX;
    break;
  }
}

bool LeadscrewStopSync::setStop(LeadscrewStopPosition position, int stopPosition,
                                int currentLeadscrewPosition, int spindlePosition) {
  switch (position) {
  case LeadscrewStopPosition::LEFT:
    leftStopPosition = stopPosition;
    leftStopState = LeadscrewStopState::SET;
    if (syncPositionState == LeadscrewSpindleSyncPositionState::UNSET && stopPosition == currentLeadscrewPosition) {
      spindleSyncPosition = spindlePosition;
      syncPositionState = LeadscrewSpindleSyncPositionState::LEFT;
      return true;
    }
    break;
  case LeadscrewStopPosition::RIGHT:
    rightStopPosition = stopPosition;
    rightStopState = LeadscrewStopState::SET;
    if (syncPositionState == LeadscrewSpindleSyncPositionState::UNSET && stopPosition == currentLeadscrewPosition) {
      spindleSyncPosition = spindlePosition;
      syncPositionState = LeadscrewSpindleSyncPositionState::RIGHT;
      return true;
    }
    break;
  }
  return false;
}

/**
 * Explicit "I am in the groove here" anchor (see Leadscrew::setSyncPoint).
 *
 * This runs on the DisplayTask (core 1) while the SpindleTask (core 0) may be
 * inside Leadscrew::update() reading these same fields with no lock. The state
 * enum is written LAST, behind a compiler barrier (no instructions emitted; it
 * only stops the compiler sinking the coordinate stores below the state store),
 * so a reader that sees MANUAL for the FIRST time necessarily sees both
 * coordinates already in memory.
 *
 * WHY A TORN READ IS SAFE ANYWAY - do not re-derive this, and note that the
 * publication order above is NOT on its own sufficient. There are two windows
 * in which update() can genuinely observe a mismatched pair:
 *
 *   W1  previous anchor was LEFT/RIGHT: between the spindleSyncPosition store
 *       and the state store, the OLD stop coordinate is paired with the NEW
 *       spindle phase (spindleSyncPosition is shared with the stop-derived
 *       anchors).
 *   W2  previous anchor was already MANUAL (re-syncing, "the last call wins"):
 *       the state store changes nothing, so it is not a publication point at
 *       all, and between the two coordinate stores a reader sees the NEW
 *       carriage coordinate against the OLD phase - an arbitrary helix.
 *
 * Neither can do harm, and the reason is not that the transient value happens
 * to be benign - it is that the ONLY writes the re-sync block performs are
 *     m_expectedPosition = m_currentPosition - pulsesToTargetSpeed;
 *     setThreadSyncState(SS_SYNC);
 * and NEITHER contains any term derived from syncPosition or
 * spindleSyncPosition. The anchor selects only WHEN the gate fires, never WHAT
 * it writes. So a torn anchor can at worst make the gate fire (or not fire) in
 * one nanosecond-wide iteration; it can never write a wrong position. A missed
 * firing is recovered on the very next iteration, because the gate is level
 * triggered and the correct pair is published by then.
 *
 * That argument, unlike a "publish the state last" argument, covers W2 as well,
 * which is why giving MANUAL its own private spindle-phase field would NOT be
 * an improvement: it closes W1 but leaves W2 exactly as it is, at the cost of a
 * field and a hot-path load.
 */
void LeadscrewStopSync::setSyncPoint(int currentLeadscrewPosition, int spindlePosition) {
  manualSyncPosition = currentLeadscrewPosition;
  spindleSyncPosition = spindlePosition;
  std::atomic_signal_fence(std::memory_order_release);
  syncPositionState = LeadscrewSpindleSyncPositionState::MANUAL;
}

void Leadscrew::unsetStopPosition(LeadscrewStopPosition position) {
  m_stopSync.unsetStop(position, m_ratio, encoderPPR);
}

/**
 * Public facing api: only allows setting the stop position to the current position of the tool
 */
void Leadscrew::setStopPosition(LeadscrewStopPosition position) {
  setStopPosition(position, m_currentPosition);
}

void Leadscrew::setStopPosition(LeadscrewStopPosition position, int stopPosition) {
  // getCurrentPosition() is a pure getter (no side effects), so hoisting it out
  // of the sync guard is behaviour-identical; this is a cold, button-driven path.
  if (m_stopSync.setStop(position, stopPosition, m_currentPosition,
                         m_spindle->getCurrentPosition())) {
    m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_SYNC);
  }
}

/**
 * Anchor the thread helix to the CURRENT carriage position and the CURRENT
 * spindle angle. Cold path: menu action only. Contract in leadscrew.h.
 *
 * Both coordinates are sampled here, in one call, precisely so a caller cannot
 * read them at two different instants and hand in a skewed pair (half a pitch of
 * error that only shows up in the metal). No stop is created, moved or cleared.
 *
 * THE FOLLOWING ERROR MUST BE DISCARDED HERE, and it must be discarded BEFORE
 * SS_SYNC is published. Raising SS_SYNC is what releases update()'s re-sync
 * gate, and while that gate holds the axis (syncArmed() && SS_UNSYNC) update()
 * goes on accumulating spindle motion into m_expectedPosition (line ~258) with
 * m_currentPosition frozen - so the following error grows without bound, by a
 * whole pitch per spindle revolution of holding. When the gate fires normally it
 * throws that accumulation away:
 *     m_expectedPosition = m_currentPosition - pulsesToTargetSpeed;
 * Releasing the gate from here without the equivalent discard hands the axis a
 * large error to close, which it closes at maximum speed. Measured on the host
 * rig before this line existed: a sync taken part-way through a hold lurched the
 * carriage 100+ pulses (0.32 mm, a quarter pitch) instantly, straight into the
 * work. Pinned by SyncPointTest.SyncTakenWhileTheGateIsHoldingDoesNotLurch.
 *
 * The discard is a plain `= m_currentPosition` (zero following error) rather
 * than the gate's `- pulsesToTargetSpeed`: that lead term exists to pre-
 * accelerate the axis so it is up to speed by the time it REACHES the groove,
 * whereas the whole content of this declaration is that the tool is in the
 * groove already.
 *
 * Note this cannot be delegated to the gate by leaving the thread UNSYNC and
 * letting the gate raise SS_SYNC: because of that same lead term the gate is
 * deliberately NOT satisfied at the anchor point unless the spindle velocity
 * estimate has decayed to a hard zero, so the axis would stay gated (and the
 * screen would read unsynced) until nearly a full revolution had passed.
 *
 * RESIDUAL RACE, accepted: m_expectedPosition is otherwise written only inside
 * update(), so this cold-path store from the DisplayTask can be lost if it lands
 * inside the read-modify-write at line ~258 on the SpindleTask. That window is a
 * few instructions wide, it is only reachable at all if the user takes a sync
 * while the axis is ENGAGED and gated (the documented gesture is a stopped
 * spindle and a disengaged axis, where MM_DISABLED-at-rest is pinning
 * m_expectedPosition to m_currentPosition every iteration anyway), and losing it
 * degrades to the old behaviour rather than to anything worse. Closing it for
 * real needs either an atomic RMW or sync-state edge detection in update(), and
 * neither belongs on the hot path for a case the UI should not offer: the Sync
 * menu tile should be enabled only while the axis is disengaged.
 *
 * setStopPosition() above raises SS_SYNC with no such discard, and is safe for
 * the opposite reason - it only latches when the anchor was UNSET, so the gate
 * was never armed and no error can have accumulated behind it.
 */
void Leadscrew::setSyncPoint() {
  m_stopSync.setSyncPoint(m_currentPosition, m_spindle->getCurrentPosition());
  m_expectedPosition = m_currentPosition;
  m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_SYNC);
}

LeadscrewStopState Leadscrew::getStopPositionState(LeadscrewStopPosition position) {
  return m_stopSync.getState(position);
}

int Leadscrew::getStopPosition(LeadscrewStopPosition position) {
  return m_stopSync.getPosition(position);
}

void Leadscrew::setCurrentPosition(int position) {
  m_currentPosition = position;
}


bool Leadscrew::sendPulse() {

#ifdef ELS_USE_RMT
  rmtWrite(rmtObj, rmt_data, sizeof(rmt_data));
  return true;
#else
  uint8_t pinState = m_io->readStepPin();

  // Keep the pulse pin high as long as we're not scheduled to send a pulse
  if (pinState == 1) {
    m_io->writeStepPin(0);

  } else {
    m_io->writeStepPin(1);
  }

  return pinState == 1;
#endif
}

/**
 * Due to the cumulative nature of the pulses when stopping, we can model the
 * stopping distance as a quadratic equation.
 * This function calculates the number of pulses required to stop the leadscrew
 * from a given pulse delay
 */
int Leadscrew::getStoppingDistanceInPulses() {
  float time = m_leadscrewSpeed / m_leadscrewAccel;
  return m_leadscrewSpeed * time / 2;
}

/**
 * This will be positive for decceleration, negative for accelleration.
 *
 * This is not the absolute number of pulses, but the number of pulses gained/lost during the
 * accelleration/decelleration process.
 */
int Leadscrew::getTargetSpeedDistanceInPulses() {
  float targetSpeed = m_spindle->getEstimatedVelocityInPPS() * m_ratio;
  float speedDif = (float)m_currentDirection * m_leadscrewSpeed - targetSpeed;
  float time = abs(speedDif) * 1.5 / m_leadscrewAccel;  // Leave some margin for catching up. 
  if (m_globalState->getDebugMode()) {
    m_globalState->debugBuffer->m_currentDirection = (int)m_currentDirection;
    m_globalState->debugBuffer->m_leadscrewSpeed = m_leadscrewSpeed;
    m_globalState->debugBuffer->m_targetSpeed = targetSpeed;
    m_globalState->debugBuffer->m_speedDif = speedDif;
    m_globalState->debugBuffer->m_timeToTarget = time;
  }

  return 0 - ((speedDif)*time / 2);
}

void Leadscrew::update() {

  int64_t tm = micros();

  m_motionMode = m_globalState->getMotionMode();
  bool jogMode = (m_motionMode & MMF_JOG) == MMF_JOG;

  bool hitLeftEndstop = m_stopSync.hitLeftEndstop(m_currentPosition);
  bool hitRightEndstop = m_stopSync.hitRightEndstop(m_currentPosition);

  if (jogMode) {
    m_spindle->consumePosition(); // Consume the spindle position while we're jogging
    m_expectedPosition = m_currentPosition;
  } else {
    // Update expected position from any unconsumed spindle pulses
    m_expectedPosition = (m_expectedPosition + (float)(((float)m_spindle->consumePosition()) * m_ratio));
  }

  // How far are we from the expected position
  float pulsesToTargetSpeed = (float)getTargetSpeedDistanceInPulses();
  float positionErrorRaw = getPositionError();
  float positionError = positionErrorRaw + pulsesToTargetSpeed;

  // If we've hit an endstop, reset everything to disabled
  if (hitLeftEndstop || hitRightEndstop) {
    if ((m_motionMode == GlobalMotionMode::MM_JOG_LEFT && hitLeftEndstop)
      || (m_motionMode == GlobalMotionMode::MM_JOG_RIGHT && hitRightEndstop)
      // A synced thread arrests at whichever endstop it is travelling into: the
      // left stop for a normal (right-hand) thread, the right stop for a reverse
      // (left-hand) thread. Guard by direction so starting at the opposite stop
      // does not immediately disable.
      || (m_motionMode == GlobalMotionMode::MM_ENABLED && hitLeftEndstop && m_currentDirection == LeadscrewDirection::LEFT)
      || (m_motionMode == GlobalMotionMode::MM_ENABLED && hitRightEndstop && m_currentDirection == LeadscrewDirection::RIGHT)) {
      m_motionMode = GlobalMotionMode::MM_DISABLED;
      m_globalState->setMotionMode(GlobalMotionMode::MM_DISABLED);
      m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    }
  }


  if (m_motionMode == MM_DISABLED || m_motionMode == MM_DECELLERATE) {
    // consume position but don't move  
    // actually it will decellerate if necessary
    if (m_leadscrewSpeed == 0) {
      m_expectedPosition = (m_currentPosition);
      m_spindle->consumePosition();
      positionError = 0;
      m_currentDirection = LeadscrewDirection::UNKNOWN;
      m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
      if (m_motionMode == MM_DECELLERATE) {
        m_globalState->setMotionMode(GlobalMotionMode::MM_DISABLED);
      }
    }
  }

  LeadscrewDirection nextDirection = LeadscrewDirection::UNKNOWN;

  /**
   * Attempt to find the "next" direction to move in, if the current
   * direction is unknown i.e: at a standstill - we know we have to start
   * moving in that direction
   *
   * If the next direction is different from the current direction, we
   * should start decelerating to move in the intended direction
   */
  if ((((positionError > 1 && !jogMode) || m_motionMode == MM_JOG_RIGHT) && !hitRightEndstop) || m_motionMode == MM_INTERACTIVE_JOG_RIGHT) {
    nextDirection = LeadscrewDirection::RIGHT;
    if (m_currentDirection == LeadscrewDirection::LEFT && m_leadscrewSpeed == 0) {
      m_currentDirection = LeadscrewDirection::UNKNOWN;
    }
    if (m_currentDirection == LeadscrewDirection::UNKNOWN) {
      m_io->writeDirPin(config->dirRight());
      m_currentDirection = LeadscrewDirection::RIGHT;
    }
  } else if ((((positionError < -1 && !jogMode) || m_motionMode == MM_JOG_LEFT) && !hitLeftEndstop) || m_motionMode == MM_INTERACTIVE_JOG_LEFT) {
    nextDirection = LeadscrewDirection::LEFT;
    if (m_currentDirection == LeadscrewDirection::RIGHT && m_leadscrewSpeed == 0) {
      m_currentDirection = LeadscrewDirection::UNKNOWN;
    }
    if (m_currentDirection == LeadscrewDirection::UNKNOWN) {
      m_io->writeDirPin(config->dirLeft());
      m_currentDirection = LeadscrewDirection::LEFT;
    }
  } else {
    // m_currentDirection = LeadscrewDirection::UNKNOWN;
  }


  /**
   * If we are not in sync with the thread, if not, figure out where we should restart based on
   * the difference in position between the sync point and the current position
   */
  if (m_stopSync.syncArmed() && m_globalState->getThreadSyncState() == SS_UNSYNC && !jogMode) {
    int syncPosition = 0;
    switch (m_stopSync.syncPositionState) {
    case LeadscrewSpindleSyncPositionState::LEFT:
      syncPosition = m_stopSync.leftStopPosition;
      break;
    case LeadscrewSpindleSyncPositionState::RIGHT:
      syncPosition = m_stopSync.rightStopPosition;
      break;
    case LeadscrewSpindleSyncPositionState::MANUAL:
      // Explicit sync point: the anchor owns its carriage coordinate rather
      // than borrowing a stop's. Must NOT fall into the UNSET arm below, which
      // returns m_currentPosition and so has no phase gate at all.
      syncPosition = m_stopSync.manualSyncPosition;
      break;
    case LeadscrewSpindleSyncPositionState::UNSET:  // Can we even hit this???
      // position does not matter
      syncPosition = m_currentPosition;
      break;
    }

    int currentpos = m_spindle->getCurrentPosition();

    if (m_globalState->getThreadSyncState() != SS_SYNC) {
      // So, I think this is, how far we need to move, converted to spindle pulses, plus the spindle sync pos, mod the spindle PPM, to get the next revolution. 
      int expectedSyncPosition = positiveModulo((int)((m_currentPosition - pulsesToTargetSpeed - syncPosition) / m_ratio) + m_stopSync.spindleSyncPosition, encoderPPR);

      if (currentpos == expectedSyncPosition) {
        m_expectedPosition = m_currentPosition - pulsesToTargetSpeed; // Ensure these are aligned at the sync point. 
        m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_SYNC);
      }

    }
  }

  /**
   * determine if we should even be bothering to send a pulse
   * we know that we can short circuit this if:
   * - Our current direction is unknown
   * - The last pulse was sent recently i.e: less than the current pulse delay
   * - the sync position was previously set and we are currently not synced with the spindle
   *
   * The first two arms are unchanged (same tests, same order, same cost). The
   * third - the re-sync gate - is split out below because leaving through it
   * must not freeze the acceleration planner.
   */
  if (m_currentDirection == LeadscrewDirection::UNKNOWN
    || (tm - m_lastPulseTimestamp) < m_currentPulseDelay) {
    return;
  }

  /**
   * The re-sync gate is holding the axis: the helix anchor is armed and we are
   * out of sync, so no step may be emitted until the spindle brings the phase
   * back round to the anchor.
   *
   * We are past the pulse-interval test above, so a full pulse interval of real
   * time has elapsed. The axis is NOT moving, so the planner's commanded speed
   * must run down exactly as it would have on the deceleration ramp - one
   * decelerationStep() per pulse interval, the identical rule the pulse path
   * uses (see decelerationStep() in leadscrew.h). We advance
   * m_lastPulseTimestamp for the same reason the pulse path does: it is what
   * paces the ramp at one step per interval. No pulse is sent and
   * m_currentPosition is untouched - we are running the ramp down, not
   * pretending to move.
   *
   * WHY THIS IS NEEDED. The endstop arrest above publishes MM_DISABLED and
   * SS_UNSYNC together, and m_leadscrewSpeed used to decay ONLY inside
   * `if (sendPulse())`, which this path skips. With the spindle then stopped
   * the re-sync search never fires (it needs rotation to bring the phase onto
   * the anchor), so whatever speed the discrete deceleration ramp had left at
   * the arrest - measured up to ~310 PPS - stayed there indefinitely. The next
   * jog then started part-way up the ramp instead of from rest, and
   * getStoppingDistanceInPulses(), which is quadratic in this value, planned a
   * stopping distance for motion that was not happening. Pinned by
   * ArrestedThreadTest.*_SpeedRunsDownToZeroWhenTheSpindleStops.
   *
   * Reaching zero here is also what re-arms the MM_DISABLED/MM_DECELLERATE
   * re-pin near the top of update() (guarded on m_leadscrewSpeed == 0), which
   * then releases the direction latch and pins m_expectedPosition to
   * m_currentPosition - so the axis settles properly instead of sitting with a
   * stale following error.
   *
   * The `m_leadscrewSpeed > 0` guard keeps this a strict no-op once at rest:
   * an axis already stopped behind a held gate (the ordinary "engaged, waiting
   * for the phase to come round" case) sees exactly the old behaviour,
   * including leaving m_lastPulseTimestamp stale so the first step after the
   * gate fires goes out immediately.
   */
  if (m_stopSync.syncArmed()
    && m_globalState->getThreadSyncState() == SS_UNSYNC
    && !jogMode) {
    if (m_leadscrewSpeed > 0) {
      decelerationStep();
      m_lastPulseTimestamp = tm;
    }
    return;
  }



  // attempt to keep in sync with the leadscrew
  // if sendPulse returns true, we've actually sent a pulse
  if (sendPulse()) {
    /**
     * If we've sent a pulse, we need to update the last pulse micros for velocity calculations
     */
    m_lastFullPulseDurationMicros = (uint32_t)(tm - m_lastPulseTimestamp);
    m_lastPulseTimestamp = tm;

    /**
     * If the pulse was sent, we need to update the accumulator to keep track of the position
     */
    m_currentPosition += static_cast<int>(m_currentDirection);
    /**
     * We need to determine if we need to start decelerating to stop at the designated position
     *
     * The conditions which we need to start decelerating are:
     * - The position error is less than the stopping distance
     * - The direction has changed
     * - We're going to hit an endstop set by the user
     *
     * Since we have no set "stop point" like with gcode or otherwise we need to constantly be updating
     * the stopping distance based on the current speed and acceleration and cant plan ahead much further than this
     */
    int pulsesToStop = getStoppingDistanceInPulses();

    bool goingToHitLeftEndstop = m_stopSync.goingToHitLeftEndstop(m_currentPosition, pulsesToStop);
    bool goingToHitRightEndstop = m_stopSync.goingToHitRightEndstop(m_currentPosition, pulsesToStop);

    // if this is true we should start decelerating to stop at the
    // correct position

    bool shouldStop;
    switch (m_motionMode) {
    case MM_ENABLED:
      shouldStop = ((int)m_currentDirection * positionError) < 0 ||
        nextDirection != m_currentDirection ||
        goingToHitLeftEndstop || goingToHitRightEndstop;
      break;
    case MM_DISABLED:
    case MM_DECELLERATE:
      shouldStop = true;
      break;
    case MM_JOG_LEFT:
    case MM_JOG_RIGHT:
      shouldStop = m_leadscrewSpeed > config->jogSpeedPps() ||
        nextDirection != m_currentDirection ||
        goingToHitLeftEndstop || goingToHitRightEndstop;
      break;
    case MM_INTERACTIVE_JOG_LEFT:
    case MM_INTERACTIVE_JOG_RIGHT:
      shouldStop = m_leadscrewSpeed > ( ((config->jogSpeedPps()) *  m_globalState->getJogSpeed())) ||
        nextDirection != m_currentDirection;
      break;
    }


    if (shouldStop) {
      // Same single deceleration rule the held-gate path above runs; the
      // upper clamp it applies to m_currentPulseDelay is the one repeated at
      // the bottom of this block, so this is behaviour-identical to the three
      // lines it replaces.
      decelerationStep();
    } else {
      m_leadscrewSpeed += m_leadscrewAccel * min(m_currentPulseDelay, initialPulseDelay) / US_PER_SECOND;
      m_leadscrewSpeed = min(m_leadscrewSpeed, config->leadscrewMaxSpeedPps());
      m_currentPulseDelay = m_leadscrewSpeed == 0 ? initialPulseDelay : US_PER_SECOND / m_leadscrewSpeed;
    }

    GlobalThreadSyncState tss = m_globalState->getThreadSyncState();

    if (debugPulseCount == 0 && m_globalState->getDebugMode()) {
      m_globalState->debugBuffer->tm = (int)tm;
      m_globalState->debugBuffer->positionError = positionError;
      m_globalState->debugBuffer->positionErrorRaw = positionErrorRaw;
      m_globalState->debugBuffer->pulsesToTargetSpeed = pulsesToTargetSpeed;
      m_globalState->debugBuffer->m_currentPosition = m_currentPosition;
      m_globalState->debugBuffer->m_expectedPosition = m_expectedPosition;
      m_globalState->debugBuffer++;
    }
    if (++debugPulseCount > 10) {
      debugPulseCount = 0;
    }
    if ((m_globalState->debugBuffer - m_globalState->debugInit) * sizeof(DebugData) > 100000) {
      m_globalState->setDebugMode(false);
    }


    /**
     * Ensure that the pulse delay is within the bounds
     * of the initial pulse delay (i.e the pulse delay when moving from zero) and 0
     */
    if (m_currentPulseDelay > initialPulseDelay) {
      m_currentPulseDelay = initialPulseDelay;
    }
    if (m_currentPulseDelay < 0) {
      m_currentPulseDelay = 0;
    }
  }

}

float Leadscrew::getPositionError() {
  return m_expectedPosition - (float)m_currentPosition;
}

LeadscrewDirection Leadscrew::getCurrentDirection() {
  return m_currentDirection;
}

float Leadscrew::getEstimatedVelocityInMillimetersPerSecond() {
  return (getEstimatedVelocityInPulsesPerSecond() * leadscrewPitch) /
    motorPulsePerRevolution;
}
