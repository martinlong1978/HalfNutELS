
#include <axis.h>
#include <config.h>

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

typedef struct DebugData {
  int tm;
  float positionError;
  float positionErrorRaw;
  float pulsesToTargetSpeed;
  int m_currentPosition;
  int m_currentDirection;
  float m_expectedPosition;
  float m_leadscrewSpeed;
  float m_targetSpeed;
  float m_speedDif;
  float m_timeToTarget;
} DebugData;

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

  volatile bool m_debugMode = false;
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
    m_resyncPulseCount = 0;
    // Derived, not a hardcoded 5 - the same latent bug incJogSpeed() was fixed
    // for (docs/ux-redesign.md Sec. 4). Shrinking jogSpeeds[] would otherwise
    // leave m_jogSpeed indexing past the end until the first incJogSpeed()
    // call clamped it. Same value as before for today's 6-entry table.
    m_jogSpeed = (int)ARRAY_SIZE(jogSpeeds) - 1;
    // The last two members. GlobalState is `new`ed (main.cpp), so these are
    // heap garbage otherwise - the exact failure mode CLAUDE.md records. They
    // are only dereferenced under getDebugMode() (leadscrew.cpp), which cannot
    // currently return true because setDebugMode() is stubbed out, so the
    // garbage is unreachable *today*; un-stubbing setDebugMode() without this
    // line would turn the debug toggle into a wild pointer write from the
    // spindle hot loop.
    debugBuffer = nullptr;
    debugInit = nullptr;
  }

public:
  DebugData* volatile debugBuffer;
  DebugData* volatile debugInit;


  // singleton stuff, no cloning and no copying
  GlobalState(GlobalState const&) = delete;
  void operator=(GlobalState const&) = delete;

  bool getDebugMode();
  void setDebugMode(bool mode);

  float getJogSpeed();
  int getJogIndex();

  void incJogSpeed();
  void decJogSpeed();

  static GlobalState* getInstance();

  void IncFeedMode();
  GlobalFeedMode getFeedMode();

  void setMotionMode(GlobalMotionMode mode);
  GlobalMotionMode getMotionMode();

  void setUnitMode(GlobalUnitMode mode);
  GlobalUnitMode getUnitMode();

  void setThreadSyncState(GlobalThreadSyncState state);
  GlobalThreadSyncState getThreadSyncState();

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
