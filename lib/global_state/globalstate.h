
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
 * The state of the global button lock
 * Unlocked: The buttons are unlocked
 * Locked: The buttons are locked
 */
enum GlobalButtonLock { LK_UNSET, LK_UNLOCKED, LK_LOCKED };

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
  volatile GlobalButtonLock m_buttonLock;

  volatile bool m_debugMode = false;
  volatile bool m_displayReset = false;

  volatile int m_feedSelect;

  volatile int m_jogSpeed;

  // the position at which the spindle will be back in sync with the leadscrew
  // note that this position actually has *two* solutions, left and right
  // but we only use the "left" position and calculate the "right" position when
  // required
  volatile int m_resyncPulseCount;

  GlobalState() {
    setUnitMode(DEFAULT_UNIT_MODE);
    setButtonLock(LK_LOCKED);
    setFeedSelect(-1);
    setThreadSyncState(SS_UNSYNC);
    m_motionMode = MM_DISABLED;
    m_resyncPulseCount = 0;
    m_jogSpeed = 5;
    m_feedMode = DEFAULT_FEED_MODE;
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

  void setButtonLock(GlobalButtonLock lock);
  GlobalButtonLock getButtonLock();

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
