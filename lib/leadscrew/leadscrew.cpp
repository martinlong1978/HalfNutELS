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
  // debugPulseCount is gone with the "every 11th pulse" decimator it drove;
  // the capture's rate is now decided in DebugCapture::due(), which keeps its
  // own state (lib/global_state/debugcapture.h).
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
  // ONE item: a step is one high/low pair. The third argument of rmtWrite is
  // an ITEM COUNT (it reaches rmt_write_items() unchanged), NOT a byte count -
  // sizeof(rmt_data) is 96, so this used to transmit 96 items and read 72 of
  // them from beyond the end of the array. 96 also exceeds the channel's
  // RMT_MEM_64 block (main.cpp), so rmt_write_items had to BLOCK waiting for
  // the hardware to drain durations that were uninitialised heap. Measured on
  // the lathe: the spindle loop fell from 78 kHz to 58 Hz while jogging, and
  // recovered the instant motion stopped.
  rmtWrite(rmtObj, rmt_data, 1);
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
// The two extra parameters exist ONLY so the capture's per-iteration
// bookkeeping can live inside the `recording()` guard that is already here,
// instead of costing the disabled hot path a second guard of its own in
// update(). Both are values update() has to hand at the call site anyway, so
// passing them is free; re-deriving them here would not be (nowUs would mean a
// second micros() call, and consumePosition() must be called exactly once per
// iteration). There is exactly one caller.
int Leadscrew::getTargetSpeedDistanceInPulses(uint32_t nowUs, int spindleDelta) {
  float targetSpeed = m_spindle->getEstimatedVelocityInPPS() * m_ratio;
  float speedDif = (float)m_currentDirection * m_leadscrewSpeed - targetSpeed;
  float time = abs(speedDif) * 1.5 / m_leadscrewAccel;  // Leave some margin for catching up. 
  // Capture site 1 of 2: the per-iteration bookkeeping, plus the five SPEED
  // fields of the pending sample. Runs once per update() while a capture is
  // armed; the sample is published by site 2 (further down update()), so what
  // survives in a committed slot is always the pair of writes from ONE
  // iteration.
  //
  // noteIteration() is what measures starvation: the gap since the previous
  // iteration of the hot loop, and the spindle delta that piled up behind it.
  // Both are peak-held until the sample is committed, so decimation cannot lose
  // a stall (debugcapture.h).
  //
  // Cost when no capture is running: one volatile bool load and a
  // not-taken branch. That is the whole of it - see DebugCapture::recording().
  DebugCapture& dbg = m_globalState->debug();
  if (dbg.recording()) {
    dbg.noteIteration(nowUs, spindleDelta);
    DebugData* s = dbg.slot();
    s->m_currentDirection = (int)m_currentDirection;
    s->m_leadscrewSpeed = m_leadscrewSpeed;
    s->m_targetSpeed = targetSpeed;
    s->m_speedDif = speedDif;
    s->m_timeToTarget = time;
    s->loopGapUs = dbg.peakGapUs();
    s->spindleDelta = dbg.peakSpindleDelta();
  }

  return 0 - ((speedDif)*time / 2);
}

void Leadscrew::update() {

  int64_t tm = micros();

  m_motionMode = m_globalState->getMotionMode();
  bool jogMode = (m_motionMode & MMF_JOG) == MMF_JOG;

  bool hitLeftEndstop = m_stopSync.hitLeftEndstop(m_currentPosition);
  bool hitRightEndstop = m_stopSync.hitRightEndstop(m_currentPosition);

  // Hoisted out of the two arms below, which each called consumePosition()
  // exactly once - so this is behaviour-identical, and it gives the capture the
  // number the starvation hypothesis turns on: how many spindle counts had
  // piled up since the last iteration. A stalled loop shows up here as a spike,
  // one iteration before m_expectedPosition leaps forward by delta x ratio.
  const int spindleDelta = m_spindle->consumePosition();
  if (jogMode) {
    // Consumed above and deliberately discarded while jogging.
    m_expectedPosition = m_currentPosition;
  } else {
    // Update expected position from any unconsumed spindle pulses
    m_expectedPosition = (m_expectedPosition + ((float)spindleDelta * m_ratio));
  }

  // How far are we from the expected position
  const uint32_t nowUs = (uint32_t)tm;
  float pulsesToTargetSpeed = (float)getTargetSpeedDistanceInPulses(nowUs, spindleDelta);
  float positionErrorRaw = getPositionError();
  float positionError = positionErrorRaw + pulsesToTargetSpeed;

  // If we've hit an endstop, reset everything to disabled
  if (hitLeftEndstop || hitRightEndstop) {
    if ((m_motionMode == GlobalMotionMode::MM_JOG_LEFT && hitLeftEndstop)
      || (m_motionMode == GlobalMotionMode::MM_JOG_RIGHT && hitRightEndstop)
      // The hold-jog (issue #11) arrests here on exactly the same terms as the
      // powered run above, and deliberately by the same spelled-out equality
      // rather than an MMF_ mask: a stop being SET on that side is the ONLY
      // reason holding the arrow means this mode instead of the dead-man
      // MM_INTERACTIVE_JOG_* below, so arresting on it is the whole point of
      // the mode rather than an incidental property of the jog family.
      //
      // Direction-matched for the same reason MM_JOG_* is: the operator must
      // still be able to hold the arrow and drive OFF the stop the carriage is
      // already parked on. Only travel INTO a stop arrests.
      || (m_motionMode == GlobalMotionMode::MM_HOLD_JOG_LEFT && hitLeftEndstop)
      || (m_motionMode == GlobalMotionMode::MM_HOLD_JOG_RIGHT && hitRightEndstop)
      // A synced thread arrests at whichever endstop it is travelling into: the
      // left stop for a normal (right-hand) thread, the right stop for a reverse
      // (left-hand) thread. Guard by direction so starting at the opposite stop
      // does not immediately disable.
      //
      // THE DIRECTION LATCH ALONE IS NOT ENOUGH, and the hole it leaves is the
      // one gesture every thread pass ends in. m_currentDirection is released to
      // UNKNOWN by the MM_DISABLED/MM_DECELLERATE re-pin just below, the moment
      // the ramp reaches zero, and it is only ever ASSIGNED inside the two
      // stepping branches further down - which are themselves gated on
      // `!hitEndstop`. So once a pass has arrested on its stop and settled,
      // pressing ENABLE while still parked there produces a state nothing can
      // leave: the latch says UNKNOWN so this arrest cannot fire, the stepping
      // branch that would re-acquire the latch is blocked by the stop, and the
      // spindle goes on feeding m_expectedPosition into a following error that
      // nothing consumes (measured: 0 pulses of travel against 472 pulses of
      // banked error). That is worse than a merely stuck axis, because
      // MM_ENABLED is exactly what the panel prints as CUTTING and what
      // UiState::underPower() gates the knob, the stop edits and the menu tiles
      // on - so the machine locks the operator out of the very keys they would
      // use to recover from it.
      //
      // The second arm of each test therefore asks the question the latch was
      // only ever standing in for: is anything actually ASKING the carriage to
      // move further into the stop it is already touching? That is a demand
      // rather than a history, so it is true on the very first iteration of a
      // re-engagement and does not need a step to have been taken first. The
      // +/-1 deadband is the same one the stepping branches below apply, so the
      // arrest fires on exactly the amount of demand that would otherwise have
      // produced a step into the stop.
      //
      // IT MUST BE positionErrorRaw, NOT positionError. positionError is
      // positionErrorRaw + pulsesToTargetSpeed, and that second term is
      // feed-forward derived entirely from the spindle's VELOCITY ESTIMATE -
      // which at the instant of re-engagement is STALE, at full magnitude, and
      // pointing the way the pass that just finished was going (measured -1200
      // PPS immediately after the axis settled; Spindle only zeroes its estimate
      // after a whole second with no encoder pulses, and the settle finishes long
      // before that). So for the first fraction of a second of a pass fed AWAY
      // from a stop, positionError points INTO it - purely as pre-acceleration
      // for a rotation that has already stopped. An arrest keyed on it fires
      // there and kills the ordinary next pass: back the tool out, wind on a few
      // thou, re-engage on the stop you finished on and cut away from it. The raw
      // error carries no such term, and the separation is total rather than
      // marginal - sampled only on iterations where the carriage is touching the
      // stop, fed AWAY the worst raw error toward the stop is 0.00 pulses; fed
      // INTO it, -78.74. Pinned by
      // AFinishedPassReEngagedAndFedAwayFromItsStopStillRuns in
      // test/test_endstop_deadlock.
      //
      // TWO NEARBY FIXES ARE BOTH WRONG, and both are fenced by two-second dwell
      // tests rather than by narrow windows:
      //   - "arrest whenever the latch is UNKNOWN" - the latch is UNKNOWN every
      //     time the machine is at rest, which includes the perfectly good pass
      //     that is about to start;
      //   - "arrest whenever engaged, on a stop and unable to step" - an engaged
      //     axis waiting for the operator to reach for the spindle switch cannot
      //     step either, and has been asked for nothing.
      // Neither the latch nor the stop distinguishes a stall from either of
      // those. Only the demand does, which is why that is what this keys on.
      //
      // The MM_INTERACTIVE_JOG_* modes stay deliberately absent from all of this,
      // as they are from the `!hitEndstop` gating below: the dead-man jog drives
      // straight through a stop (measured: -38000 past a stop set at -400), and
      // it is the operator's only way to move the carriage off a stop that was
      // set in the wrong place. Adding them here is a one-line change that looks
      // like tidying and is not.
      //
      // MM_HOLD_JOG_* above is the deliberate counterpart, and the difference
      // between the two is the entire safety distinction: the hold-jog is only
      // ever reached on a side whose stop is SET, where the operator is asking
      // to approach a limit they chose, so it arrests; the interactive jog is
      // only ever reached on a side with NO stop, or as the escape from one in
      // the wrong place, so it does not. Since issue #11 that escape is no
      // longer available on a side that HAS a stop - unsetting the stop first
      // is the route, and that is permitted at rest (docs/ux-redesign.md Sec. 3).
      || (m_motionMode == GlobalMotionMode::MM_ENABLED && hitLeftEndstop
        && (m_currentDirection == LeadscrewDirection::LEFT || positionErrorRaw < -1))
      || (m_motionMode == GlobalMotionMode::MM_ENABLED && hitRightEndstop
        && (m_currentDirection == LeadscrewDirection::RIGHT || positionErrorRaw > 1))) {
      m_motionMode = GlobalMotionMode::MM_DISABLED;
      m_globalState->setMotionMode(GlobalMotionMode::MM_DISABLED);
      m_globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    }
  }


  if (m_motionMode == MM_DISABLED || m_motionMode == MM_DECELLERATE || m_motionMode == MM_UNSET) {
    // consume position but don't move
    // actually it will decellerate if necessary
    // MM_UNSET joins MM_DISABLED here for the same reason it already does at
    // the six !=-MM_DISABLED-and-!=-MM_UNSET call sites elsewhere (buttonpad,
    // DebugSink, WebSettings, the display state bar, the screenshot scenes):
    // MM_UNSET is not an unknown value, it's the named "nothing commanded"
    // state, and this is where that meaning gets enforced for real rather
    // than just read.
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
   *
   * WHERE A MODE SITS RELATIVE TO `&& !hitEndstop` IS THE WHOLE DIFFERENCE
   * BETWEEN THE TWO JOGS, and it is not a formatting choice. A mode named
   * INSIDE the bracket is gated by the stop: once hitRightEndstop is true this
   * branch stops selecting RIGHT, no direction is latched, no step is emitted,
   * and the arrest above finishes the job. A mode OR'd in OUTSIDE the bracket -
   * MM_INTERACTIVE_JOG_* alone - keeps selecting its direction with the stop
   * standing on it, which is exactly how the dead-man jog drives through a
   * misplaced stop.
   *
   * MM_HOLD_JOG_* (issue #11) therefore goes INSIDE, alongside MM_JOG_*: it is
   * the hold gesture on a side whose stop is SET, and it must arrest there. It
   * differs from MM_JOG_* only in speed, which is settled in the shouldStop
   * switch below, not here.
   */
  if ((((positionError > 1 && !jogMode) || m_motionMode == MM_JOG_RIGHT || m_motionMode == MM_HOLD_JOG_RIGHT) && !hitRightEndstop) || m_motionMode == MM_INTERACTIVE_JOG_RIGHT) {
    nextDirection = LeadscrewDirection::RIGHT;
    if (m_currentDirection == LeadscrewDirection::LEFT && m_leadscrewSpeed == 0) {
      m_currentDirection = LeadscrewDirection::UNKNOWN;
    }
    if (m_currentDirection == LeadscrewDirection::UNKNOWN) {
      m_io->writeDirPin(config->dirRight());
      m_currentDirection = LeadscrewDirection::RIGHT;
    }
  } else if ((((positionError < -1 && !jogMode) || m_motionMode == MM_JOG_LEFT || m_motionMode == MM_HOLD_JOG_LEFT) && !hitLeftEndstop) || m_motionMode == MM_INTERACTIVE_JOG_LEFT) {
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

  // Capture site 2 of 2: the six POSITION fields, and the commit that publishes
  // the sample.
  //
  // PLACED HERE, BEFORE THE EARLY RETURNS, NOT INSIDE `if (sendPulse())` where
  // it used to be. The old placement could only record on an iteration that
  // actually emitted a step, so a stall while the axis was HELD - by the
  // re-sync gate, or by the pulse-interval wait - produced no rows at all. That
  // is exactly the case worth seeing: the re-sync gate is the prime suspect if
  // the trace shows an error spike with a flat loop gap. Every iteration is now
  // eligible, and DebugCapture::due() decides.
  //
  // Everything read below is settled for this iteration by this point:
  // positionError and its two components, m_expectedPosition, and
  // m_currentDirection (resolved by the direction block above). m_currentPosition
  // is the pre-pulse value - at most one pulse behind what the old site
  // recorded, which is immaterial next to a 400-pulse reversal.
  //
  // NOTHING IS ADDED TO THE HOT PATH WHEN NO CAPTURE IS RUNNING. The disabled
  // cost is one volatile bool load and a not-taken branch, and it is strictly
  // LESS than the code this replaced: that ran `++debugPulseCount` and then a
  // two-pointer-load, subtract, multiply, compare buffer-full test on EVERY
  // pulse, both outside the getDebugMode() guard. The decimator's state and the
  // full test now both live inside the branch. (Binding the reference below
  // compiles to nothing - it is the same singleton pointer the line above it
  // already holds.)
  DebugCapture& dbg = m_globalState->debug();
  if (dbg.recording()) {
    const int dir = (int)m_currentDirection;
    if (dbg.due(nowUs, dir)) {
      DebugData* s = dbg.slot();
      s->tm = dbg.relativeMicros(nowUs);
      s->positionError = positionError;
      s->positionErrorRaw = positionErrorRaw;
      s->pulsesToTargetSpeed = pulsesToTargetSpeed;
      s->m_currentPosition = m_currentPosition;
      s->m_expectedPosition = m_expectedPosition;
      // Advances the cursor, opens a fresh peak-hold window, and stops the
      // capture (state DBG_FULL) when the buffer is full. It never calls back
      // into GlobalState and never frees anything - the upload happens later,
      // from another task, once the carriage is at rest.
      dbg.commit(nowUs, dir);
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
    // MM_UNSET falls through to the same rule as MM_DISABLED, not as a
    // defensive default for a value the switch doesn't recognise: it's a
    // named state meaning "nothing commanded", and six sites elsewhere in
    // the codebase (buttonpad.cpp x2, DebugSink.cpp, WebSettings.cpp,
    // ST7789_320_240displaylvgl.cpp, tools/screenshot/src/scenes.cpp) already
    // treat MM_UNSET and MM_DISABLED as equivalent. An explicit case (not
    // `default:`) would let -Wswitch flag the next enum value someone adds -
    // IF -Wall were enabled here. It isn't (platformio.ini's build_flags for
    // esp32dev_usb/esp32dev_publish are empty; native passes -Wp,-w). So this
    // is future-proofing for a warning that isn't currently armed, not a
    // check running today.
    // Paired with the MM_UNSET addition to the rest/re-pin block above: this
    // switch brings a mode entered at speed down to zero, and the re-pin
    // then pins it there, the same two-step MM_DECELLERATE already relies on.
    //
    // THIS ARM CARRIES NO goingToHit*Endstop TERM AND DOES NOT NEED ONE. That
    // looks like an omission next to the MM_ENABLED and jog arms above, and it
    // was very nearly "fixed" as one during review. Those terms exist to start
    // braking EARLY so the axis lands on the stop instead of arriving at speed -
    // they turn shouldStop true before it otherwise would be. Here it is already
    // unconditionally true, and decelerationStep() ramps down at m_leadscrewAccel,
    // the single rate getStoppingDistanceInPulses() itself assumes (v^2/2a). The
    // axis is therefore already stopping as fast as it physically can, and a
    // lookahead term could only set a flag that is set.
    //
    // Entering any of these three modes AT SPEED with a stop closer than the
    // stopping distance will overshoot it, and no arrangement of this switch can
    // prevent that - it is a property of the deceleration rate, not of the
    // decision. The sync bookkeeping still happens: the re-pin block publishes
    // SS_UNSYNC on reaching zero, so an overshoot cannot be mistaken for a valid
    // position. This is also why the endstop-arrest block (~377-471) has no
    // clause for these modes and must not be given one.
    case MM_UNSET:
    case MM_DISABLED:
    case MM_DECELLERATE:
      shouldStop = true;
      break;
    // ISSUE #14: goingToHitLeftEndstop/goingToHitRightEndstop are direction-
    // agnostic by construction (leadscrew_stopsync.h) - each only compares a
    // lookahead position to ITS OWN stop, with no notion of which way the
    // carriage is travelling. Left unguarded here, a stop behind the
    // carriage throttles the ramp exactly as hard as the one ahead: jog
    // RIGHT away from a LEFT stop you just arrested on, and the rear
    // predicate keeps asking "how soon can I stop AT the left stop" every
    // iteration, which the acceleration branch above (shouldStop when
    // m_leadscrewSpeed exceeds the jog cap) has to satisfy alongside it.
    //
    // Gating each term on nextDirection - RIGHT can only be blocked by the
    // stop ahead of rightward travel, LEFT only by the one ahead of
    // leftward travel - restores that: only the stop actually in front of
    // the carriage can hold the ramp down. Measured severity (issue #14
    // comment): with the gate absent this was a ~1-1.5% transient that
    // converged to exact parity within ~0.3 s, not the sustained crawl the
    // issue first suspected - it very nearly cancels because this branch's
    // speed cap and getStoppingDistanceInPulses() both scale on the same
    // m_leadscrewAccel. That near-cancellation is coincidence, not
    // intended behaviour: retune deceleration away from acceleration and
    // the rear stop starts genuinely throttling, so the fix stays even
    // though today's cost is small.
    //
    // MUST be nextDirection, NOT m_currentDirection. nextDirection is
    // computed once, unambiguously, by the direction-selection block above
    // this switch, before either latch runs. m_currentDirection starts each
    // update() UNKNOWN until that same block latches it - on the SAME call
    // as the first pulse - so a match against it evaluated any earlier
    // would see UNKNOWN, fail to match either arm, and silently drop BOTH
    // endstop terms (including the forward one) for however long the
    // mismatch lasted: a jog made fast by deleting a safety check.
    // HoldJogFromRestWithACloseForwardStopStillDeceleratesAndLandsOnIt pins
    // the forward stop still catching a jog started from rest for exactly
    // this reason.
    //
    // Carriage already sitting ON the forward stop when this mode is
    // (re)selected does not reach here at all: the endstop-arrest block
    // above (~377-471) matches this exact mode+hit combination first and
    // rewrites m_motionMode to MM_DISABLED before the switch runs, which
    // takes the unconditional-shouldStop arm below instead - so there is no
    // window where nextDirection's UNKNOWN (direction selection gates the
    // jog arms on !hitRightEndstop/!hitLeftEndstop) is asked to catch it.
    //
    // Deliberately NOT touching MM_ENABLED: a pass starts from rest (issue
    // #8), so the rear predicate there would need pulsesToStop(v) >= d0 + s
    // with d0 >= 0, which v <= sqrt(2as) never satisfies - it is already a
    // no-op on that arm, so widening this fix to it buys nothing and risks
    // the thread pass.
    //
    // Cost: two enum compares (`nextDirection == LeadscrewDirection::LEFT`
    // / `::RIGHT`), no divide, no allocation, no virtual call - and only
    // evaluated inside `if (sendPulse())`, i.e. on an iteration that
    // already emitted a pulse.
    case MM_JOG_LEFT:
    case MM_JOG_RIGHT:
      shouldStop = m_leadscrewSpeed > config->jogSpeedPps() ||
        nextDirection != m_currentDirection ||
        (nextDirection == LeadscrewDirection::LEFT && goingToHitLeftEndstop) ||
        (nextDirection == LeadscrewDirection::RIGHT && goingToHitRightEndstop);
      break;
    case MM_INTERACTIVE_JOG_LEFT:
    case MM_INTERACTIVE_JOG_RIGHT:
      shouldStop = m_leadscrewSpeed > ( ((config->jogSpeedPps()) *  m_globalState->getJogSpeed())) ||
        nextDirection != m_currentDirection;
      break;
    // The hold-jog is the one mode that takes half its rule from each of the
    // two above, which is why it needed a mode of its own rather than reusing
    // one: the SPEED CAP is the interactive jog's, because the jog-speed
    // multiplier is the control the operator has for how fast a held jog
    // moves and a jog that ignores it is not the feature (issue #11); the
    // ENDSTOP TERMS are the powered run's, because this gesture only exists on
    // a side whose stop is SET and must plan its deceleration to land on it
    // rather than arrive at speed. MM_INTERACTIVE_JOG_* omits those two terms
    // deliberately - it is allowed through a stop - and MM_JOG_* omits the
    // multiplier deliberately, since the click-run is a return-to-stop at the
    // configured jog speed rather than something the operator is steering.
    //
    // Cost is one float multiply and one GlobalState read, identical to the
    // MM_INTERACTIVE_JOG_* case, and only on an iteration that actually
    // emitted a pulse. No divide is introduced.
    //
    // Same issue #14 direction-match as MM_JOG_* above, and for the same
    // reason: the ENDSTOP TERMS are borrowed from the powered run precisely
    // so this mode plans its deceleration onto the stop it is approaching,
    // not the one it just left. See the long comment on MM_JOG_LEFT/RIGHT
    // for the full reasoning (rear-stop cancellation being coincidental,
    // why nextDirection and not m_currentDirection, why MM_ENABLED is
    // excluded) - it applies here unchanged.
    case MM_HOLD_JOG_LEFT:
    case MM_HOLD_JOG_RIGHT:
      shouldStop = m_leadscrewSpeed > (config->jogSpeedPps() * m_globalState->getJogSpeed()) ||
        nextDirection != m_currentDirection ||
        (nextDirection == LeadscrewDirection::LEFT && goingToHitLeftEndstop) ||
        (nextDirection == LeadscrewDirection::RIGHT && goingToHitRightEndstop);
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
