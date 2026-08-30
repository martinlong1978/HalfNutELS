#include <cstdint>

#pragma once

/**
 * The state of the leadscrew stop position for either the left or right stop
 */
enum class LeadscrewStopState { SET, UNSET };
enum class LeadscrewStopPosition { LEFT, RIGHT };

/**
 * The state of the spindle sync position
 * The spindle sync position is a known position of the spindle that syncs with the current thread
 * Since the spindle is a "rotational" axis and the leadscrew is a "linear" axis, we need to know
 * an anchor point of where the spindle and the leadscrew are both in sync.
 *
 * We reuse the endstop states for this, since they are similar in nature and keep the first one that is set
 *
 * MANUAL is the explicit "pick up an existing thread" anchor declared by
 * Leadscrew::setSyncPoint(). Unlike LEFT/RIGHT it does NOT borrow a stop's
 * carriage coordinate - it owns its own (manualSyncPosition) - so it is
 * independent of the stops and survives setStop()/unsetStop() untouched. It is
 * appended (not inserted) so the existing enumerator values are unchanged.
 */

enum class LeadscrewSpindleSyncPositionState { LEFT, RIGHT, UNSET, MANUAL };

/**
 * Owns the cold (button-event driven) stop-position and spindle-sync state that
 * used to live directly on Leadscrew.
 *
 * Plain struct, plain fields, no virtuals: Leadscrew::update() reads the fields
 * directly (or through the trivial header-inline predicates below), so the hot
 * path cost is identical to the old direct member access. The mutators
 * (setStop / unsetStop) are only ever called from button handlers, so they are
 * defined out of line in leadscrew.cpp.
 */
struct LeadscrewStopSync {
  // we may want more sophisticated control over positions, but for now this is
  // fine
  LeadscrewStopState leftStopState;
  int leftStopPosition;

  LeadscrewStopState rightStopState;
  int rightStopPosition;

  LeadscrewSpindleSyncPositionState syncPositionState;
  int spindleSyncPosition;

  // Carriage coordinate of an explicit (MANUAL) sync anchor. Only meaningful
  // while syncPositionState == MANUAL; the LEFT/RIGHT anchors take their
  // carriage coordinate from the stop instead.
  int manualSyncPosition;

  // Leadscrew is heap-allocated in main.cpp; these must not start from stale
  // memory or setStop()'s "sync only when UNSET" guard behaves
  // non-deterministically.
  LeadscrewStopSync()
      : leftStopState(LeadscrewStopState::UNSET),
        leftStopPosition(INT32_MIN),
        rightStopState(LeadscrewStopState::UNSET),
        rightStopPosition(INT32_MAX),
        syncPositionState(LeadscrewSpindleSyncPositionState::UNSET),
        spindleSyncPosition(0),
        manualSyncPosition(0) {}

  /**
   * Cold API (button events).
   *
   * setStop records the stop position; if no spindle-sync anchor is set yet and
   * the stop is being placed at the current leadscrew position it also records
   * spindlePosition as the sync anchor and returns true (the caller is then
   * responsible for flagging the thread as synced). Defined in leadscrew.cpp.
   */
  bool setStop(LeadscrewStopPosition position, int stopPosition,
               int currentLeadscrewPosition, int spindlePosition);
  void unsetStop(LeadscrewStopPosition position, float ratio, int encoderPPR);

  /**
   * Record an explicit (MANUAL) sync anchor: this carriage position is on the
   * helix at this spindle angle. Both coordinates are supplied by the caller in
   * one call, sampled at one instant - see Leadscrew::setSyncPoint(). Overwrites
   * any previous anchor (stop-derived or manual): the last declaration wins.
   * Touches no stop. Defined in leadscrew.cpp.
   */
  void setSyncPoint(int currentLeadscrewPosition, int spindlePosition);

  LeadscrewStopState getState(LeadscrewStopPosition position) const {
    switch (position) {
    case LeadscrewStopPosition::LEFT:
      return leftStopState;
    case LeadscrewStopPosition::RIGHT:
      return rightStopState;
    }
    return rightStopState;
  }

  int getPosition(LeadscrewStopPosition position) const {
    // todo better default values when unset
    switch (position) {
    case LeadscrewStopPosition::LEFT:
      if (leftStopState == LeadscrewStopState::SET) {
        return leftStopPosition;
      }
      return INT32_MIN;
    case LeadscrewStopPosition::RIGHT:
      if (rightStopState == LeadscrewStopState::SET) {
        return rightStopPosition;
      }
      return INT32_MAX;
    }
    return 0;
  }

  /**
   * Hot-path predicates: trivial single-expression, header-inline field reads,
   * same generated code as the old open-coded conditions in update().
   */
  bool hitLeftEndstop(int currentPosition) const {
    return leftStopState == LeadscrewStopState::SET &&
      currentPosition <= leftStopPosition;
  }

  bool hitRightEndstop(int currentPosition) const {
    return rightStopState == LeadscrewStopState::SET &&
      currentPosition >= rightStopPosition;
  }

  bool goingToHitLeftEndstop(int currentPosition, int pulsesToStop) const {
    return leftStopState == LeadscrewStopState::SET &&
      currentPosition - pulsesToStop <= leftStopPosition;
  }

  bool goingToHitRightEndstop(int currentPosition, int pulsesToStop) const {
    return rightStopState == LeadscrewStopState::SET &&
      currentPosition + pulsesToStop >= rightStopPosition;
  }

  bool syncArmed() const {
    return syncPositionState != LeadscrewSpindleSyncPositionState::UNSET;
  }
};
