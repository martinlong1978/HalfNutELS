
#include <axis.h>
#include <config.h>
// The motion-trace capture buffer (DebugData, DebugCapture, DebugCaptureState).
// It used to be two raw pointers and a struct declared here; it is its own
// class now so the buffer it writes through is actually allocated, and so the
// decimator and bookkeeping can be host-tested. See debugcapture.h.
#include "debugcapture.h"

#pragma once

#ifdef ELS_UI_ENCODER
enum EncoderColour { EC_NONE = 0, EC_RED = 1, EC_GREEN = 2, EC_YELLOW = 3 };
#endif

// Major modes are the main modes of the application, like the feed or thread
// The spindle acts the same way in both threading and feeding mode
// this is just for the indicator on the screen
enum GlobalFeedMode { FM_UNSET = -1, FM_FEED = 0, FM_THREAD = 1, FM_JOG = 2, FM_THREAD_REVERSE = 3 };

// The motion mode of the leadscrew in relation to the spindle
// Disabled: The leadscrew does not move when the spindle is moving
// Jog: The leadscrew is moving independently of the spindle
// Enabled: The leadscrew is moving in sync with the spindle
enum GlobalMotionMode { MM_UNSET = 0, MM_DISABLED = 2, MM_ENABLED = 3, MM_JOG_LEFT = 4, MM_JOG_RIGHT = 5, MM_INTERACTIVE_JOG_LEFT = 12, MM_INTERACTIVE_JOG_RIGHT = 13, MM_DECELLERATE = 20};

enum GlobalMotionModeMasks { MMF_ENABLESTATE = 2, MMF_JOG = 4, MMF_JOG_DIRECTION = 5, MMF_INTERACTIVEJOG = 8};

/**
 * The unit mode of the application, usually for threading
 * Choose either the superior metric system or the deprecated imperial system
 */
enum GlobalUnitMode { METRIC, IMPERIAL };

/**
 * The state of the global thread sync
 * Sync: The spindle and leadscrew are in sync
 * Unsync: The spindle and leadscrew are out of sync
 * Resync:
 */
enum GlobalThreadSyncState { SS_UNSET, SS_SYNC, SS_UNSYNC };

/**
 * OTA update progress, set by the OTA task (SpindleTask) and read by the
 * DisplayTask so the OTA screen can show the right message. The two tasks
 * coordinate only through this (volatile) value; no display method is ever
 * called from the OTA task.
 *  OTA_IDLE        - no update in progress
 *  OTA_CHECKING    - connecting / checking the latest release version
 *  OTA_NO_UPDATE   - already on the latest version, nothing to do
 *  OTA_DOWNLOADING - downloading + flashing the new image (progress bar active)
 *  OTA_FAILED      - the update failed (device will reboot)
 */
enum GlobalOtaStatus { OTA_IDLE, OTA_CHECKING, OTA_NO_UPDATE, OTA_DOWNLOADING, OTA_FAILED };

/**
 * The stepper driver's alarm, published by the alarm task (src/alarm.cpp) and
 * read by the DisplayTask - the display, to raise the modal, and ButtonPad, to
 * put UiState into UiFocus::Alarm. The decision logic is all in
 * lib/alarm/alarmmonitor.h; these three values are its state() mirrored onto
 * the coordination bus so nothing outside that task touches the object itself.
 *  AS_CLEAR    - no fault
 *  AS_ALARM    - a fault has been LATCHED. Motion is inhibited and the modal is
 *                up until the operator acknowledges it, whether or not the
 *                driver is still asserting.
 *  AS_CLEARING - the acknowledgement is in flight: ENA is being pulsed and the
 *                drivers are off. Still inhibited; the modal stays up.
 */
enum GlobalAlarmState { AS_CLEAR = 0, AS_ALARM = 1, AS_CLEARING = 2 };

// this is a singleton class - we don't want more than one of these existing at
// a time!
class GlobalState {
private:
  static GlobalState* m_instance;
  volatile bool OTA = false;
  volatile int OTAbytes = 0;
  volatile int OTAlength = 0;
  volatile GlobalOtaStatus m_otaStatus = OTA_IDLE;



  // These are read AND written from two RTOS tasks pinned to different cores:
  // the SpindleTask (core 0, via Leadscrew::update()) and the DisplayTask
  // (core 1, buttons + display). They are 32-bit aligned, so reads/writes are
  // atomic on the ESP32; `volatile` stops the compiler caching stale copies
  // across the tasks. No lock is taken, so the spindle hot loop is unaffected.
  volatile GlobalFeedMode m_feedMode;
  volatile GlobalMotionMode m_motionMode;
  volatile GlobalUnitMode m_unitMode;
  volatile GlobalThreadSyncState m_threadSyncState;

  // Written by the alarm task (core 0), read by the DisplayTask. 32-bit and
  // aligned, so the read is atomic on the ESP32, and `volatile` is doing its
  // usual job: stopping the display from caching a stale copy across its
  // 100 ms loop. The alarm task owns this field outright - unlike m_motionMode,
  // which it also writes (see the note on setMotionMode below).
  volatile GlobalAlarmState m_alarmState;
  // Two facts the STATE word above does not carry, and that only the modal
  // needs: whether the driver is asserting a fault right now (as opposed to
  // having latched one that has since gone away), and whether the last reset
  // attempt finished with it still faulted. They are what let the dialog say
  // "clear the fault" or "press OK" or "that did not work" instead of showing
  // one blank wall for all three. Published by the same task, in the same
  // call, as m_alarmState.
  volatile bool m_alarmFaultPresent;
  volatile bool m_alarmClearFailed;

  // The motion-trace capture. Not a bool any more: the "is debug on" flag and
  // the buffer that flag lets the hot loop write through are one object, so
  // they cannot get out of step (see the constructor note below).
  //
  // Held BY VALUE - no pointer, no allocation at construction. The trace
  // buffer itself is allocated by arm() and freed by discard(); this object is
  // 40-odd bytes of bookkeeping that exists from boot.
  DebugCapture m_debug;

  volatile bool m_displayReset = false;

  volatile int m_feedSelect;

  volatile int m_jogSpeed;

  // the position at which the spindle will be back in sync with the leadscrew
  // note that this position actually has *two* solutions, left and right
  // but we only use the "left" position and calculate the "right" position when
  // required
  volatile int m_resyncPulseCount;

  // Per-(unit, mode-type) remembered pitch-select index, per
  // docs/ux-redesign.md Sec. 4 ("MODE"): four independent slots so switching
  // feed mode / unit restores the last index used for that slot instead of
  // resetting to the default. Indexed [unit][isThread]; FM_THREAD_REVERSE
  // shares the "thread" slot for the current unit (same pitch tables).
  //
  // CROSS-TASK SAFETY. Unlike the scalars above this is an ARRAY, and every
  // mutation of it is a read-modify-write across several fields (read
  // m_unitMode, read m_feedMode, index, store). `volatile` gives no atomicity
  // over that sequence, so it is NOT what makes this safe. What makes it safe
  // is that the whole feed/pitch group - m_feedMode, m_unitMode, m_feedSelect,
  // m_pitchMemory - has a SINGLE WRITER: the DisplayTask (core 1). Traced
  // callers of every mutator:
  //   IncFeedMode()          <- ButtonPad::modeCycleHandler   (DisplayTask)
  //   setUnitMode()          <- ButtonPad::modeCycleHandler   (DisplayTask)
  //   next/prevFeedPitch()   <- ButtonPad::rate*Handler       (DisplayTask)
  //                          <- KeyArray::updateEncoderPos, reached only from
  //                             consumeButton() on the DisplayTask - NOT from
  //                             the GPIO/timer ISRs, which touch no GlobalState
  //   setFeedSelect()        <- private, only from the above + the ctor
  // The SpindleTask (core 0) never calls any of them and never reads
  // m_feedSelect / m_feedMode / m_unitMode / m_pitchMemory in its hot loop;
  // Leadscrew::update() gets the pitch pushed to it by the DisplayTask via
  // setTargetPitchMM(), and the only field of this group the spindle path
  // reads is the plain 32-bit m_jogSpeed (leadscrew.cpp getJogSpeed()).
  // Leadscrew's own getCurrentFeedPitch() call is in its constructor, which
  // runs in setup() before either task exists.
  // KEEP IT THAT WAY: if a second task (web handler, OTA, a future UI task)
  // ever needs to change mode/unit/pitch it must post an intent for the
  // DisplayTask to apply, not call these directly - adding a lock here is not
  // an option (see CLAUDE.md, no locks on the hot path).
  // `volatile` is retained because the DisplayTask's writes to m_feedMode /
  // m_unitMode / m_feedSelect are still observed by display code and must not
  // be cached stale.
  volatile int m_pitchMemory[2][2];

  static int pitchMemoryUnitIndex(GlobalUnitMode unit) {
    return unit == METRIC ? 0 : 1;
  }
  static int pitchMemoryTypeIndex(GlobalFeedMode mode) {
    return (mode == FM_THREAD || mode == FM_THREAD_REVERSE) ? 1 : 0;
  }

  GlobalState() {
    // Order matters: m_feedMode and m_unitMode must be valid before any call
    // that reads them (getCurrentFeedSelectArraySize(), pitchMemory index
    // helpers), and m_pitchMemory must be initialised before setFeedSelect()
    // writes back into it.
    m_feedMode = DEFAULT_FEED_MODE;
    m_unitMode = DEFAULT_UNIT_MODE;

    m_pitchMemory[pitchMemoryUnitIndex(METRIC)][0] = DEFAULT_METRIC_FEED_PITCH_IDX;
    m_pitchMemory[pitchMemoryUnitIndex(METRIC)][1] = DEFAULT_METRIC_THREAD_PITCH_IDX;
    m_pitchMemory[pitchMemoryUnitIndex(IMPERIAL)][0] = DEFAULT_IMPERIAL_FEED_PITCH_IDX;
    m_pitchMemory[pitchMemoryUnitIndex(IMPERIAL)][1] = DEFAULT_IMPERIAL_THREAD_PITCH_IDX;

    // Seed from the slot the configured default mode/unit actually selects.
    // Passing DEFAULT_METRIC_FEED_PITCH_IDX here instead would hardcode the
    // METRIC/FEED default onto whatever slot DEFAULT_UNIT_MODE /
    // DEFAULT_FEED_MODE happen to name, and stamp that wrong value into
    // m_pitchMemory via setFeedSelect()'s write-back. Harmless today only
    // because all four DEFAULT_*_PITCH_IDX are 8.
    setFeedSelect(
        m_pitchMemory[pitchMemoryUnitIndex(m_unitMode)][pitchMemoryTypeIndex(m_feedMode)]);
    setThreadSyncState(SS_UNSYNC);
    m_motionMode = MM_DISABLED;
    m_alarmState = AS_CLEAR;
    m_alarmFaultPresent = false;
    m_alarmClearFailed = false;
    m_resyncPulseCount = 0;
    // Derived, not a hardcoded 5 - the same latent bug incJogSpeed() was fixed
    // for (docs/ux-redesign.md Sec. 4). Shrinking jogSpeeds[] would otherwise
    // leave m_jogSpeed indexing past the end until the first incJogSpeed()
    // call clamped it. Same value as before for today's 6-entry table.
    m_jogSpeed = (int)ARRAY_SIZE(jogSpeeds) - 1;
    // m_debug is the last member, and it initialises itself: DebugCapture's
    // constructor sets every one of its own fields (debugcapture.cpp). It has
    // to, for the same reason everything above is set explicitly - GlobalState
    // is `new`ed in main.cpp and the heap is not zeroed - and it matters more
    // here than anywhere else, because the members concerned are the write
    // cursor the SpindleTask dereferences.
    //
    // The two raw `DebugData*` members that used to live here are gone. They
    // were nulled by this constructor but NOTHING EVER ALLOCATED THEM: the old
    // setDebugMode() was an empty body, so turning the debug toggle back on
    // would have made leadscrew.cpp write through a null cursor from the
    // spindle hot loop. DebugCapture owns the allocation and the enable flag
    // together, which is what makes that state unreachable.
  }

public:

  // singleton stuff, no cloning and no copying
  GlobalState(GlobalState const&) = delete;
  void operator=(GlobalState const&) = delete;

  // --- Motion-trace capture -------------------------------------------------
  //
  // The hot loop's view. Kept as getDebugMode() because that is the name the
  // two capture sites in Leadscrew::update() have always used, and keeping it
  // means the guard around them is untouched by this change.
  bool getDebugMode();

  // Arm (true) or discard (false) a capture. Cold path only - the DisplayTask,
  // from the "Debug capture" menu tile. arm() allocates ~100 KB, which is
  // precisely why the tile is one menuTileBlock() refuses while the carriage
  // is under power.
  void setDebugMode(bool mode);

  // The capture object itself, for the two Leadscrew::update() sites (hot
  // path, all inline), the uploader and the Diagnostics readout.
  DebugCapture& debug() { return m_debug; }

  float getJogSpeed();
  int getJogIndex();

  void incJogSpeed();
  void decJogSpeed();

  static GlobalState* getInstance();

  void IncFeedMode();
  GlobalFeedMode getFeedMode();

  // THREE writers, and that is deliberate rather than an accident of growth:
  //   * the DisplayTask, from ButtonPad, for everything the operator commands;
  //   * the SpindleTask, from Leadscrew::update(), for the endstop arrest;
  //   * the alarm task, which HOLDS this at MM_DISABLED for the whole of a
  //     stepper alarm (src/alarm.cpp).
  // All three only ever publish a single aligned 32-bit value, and the third
  // only ever writes MM_DISABLED - the most restrictive value there is - so a
  // race between it and either of the others cannot produce motion that nobody
  // asked for. It holds the value rather than writing it once on the trip,
  // which is what closes the window between the fault and the panel finding
  // out about it on its next 100 ms pass.
  void setMotionMode(GlobalMotionMode mode);
  GlobalMotionMode getMotionMode();

  void setUnitMode(GlobalUnitMode mode);
  GlobalUnitMode getUnitMode();

  void setThreadSyncState(GlobalThreadSyncState state);
  GlobalThreadSyncState getThreadSyncState();

  // The stepper alarm, mirrored off AlarmMonitor by the alarm task. Read
  // freely; the ONLY legitimate writer is that task (src/alarm.cpp), because
  // the latch it describes lives in the AlarmMonitor and this is only a copy.
  // All three published together, from the one call site in src/alarm.cpp, so
  // the modal can never render a state word against detail from a different
  // sample. `faultPresent` is the debounced input; `clearFailed` says the last
  // reset attempt ended with the driver still faulted.
  void setAlarmState(GlobalAlarmState state, bool faultPresent,
                     bool clearFailed);
  GlobalAlarmState getAlarmState();
  bool getAlarmFaultPresent();
  bool getAlarmClearFailed();
  // "Anything but clear" - the predicate every consumer actually wants. The
  // modal is up and motion is inhibited for AS_CLEARING as much as for
  // AS_ALARM, since the drivers are switched off during the reset pulse.
  bool alarmActive();

  bool hasOTA();
  void setOTA();
  void clearOTA();

  void setOTABytes(int bytes);
  int getOTABytes();
  int getOTALength();
  void setOTAContentLength(int length);

  void setOtaStatus(GlobalOtaStatus status);
  GlobalOtaStatus getOtaStatus();

  void setDisplayReset();
  bool getDisplayReset();

  void setFeedSelect(int select);
  int getFeedSelect();
  float getCurrentFeedPitch();
  int nextFeedPitch();
  int prevFeedPitch();
  
protected:
  int getCurrentFeedSelectArraySize();
};
