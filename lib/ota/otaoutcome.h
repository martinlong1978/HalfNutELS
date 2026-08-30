// How an OTA attempt ended, and what the machine owes the operator because of it.
//
// Pure C++ on purpose: NO Arduino / ESP / FreeRTOS / WiFi / LVGL includes, so it
// builds and is unit-tested on the native host (`pio test -e native`), exactly
// like lib/alarm, lib/keyscan and lib/ui. It opens no socket, writes no flash and
// reboots nothing - src/ESPCommsManager.cpp does all three, and this class
// decides everything in between. Time arrives as a nowMs parameter; the class
// owns no clock (same bargain as AlarmMonitor).
//
// WHY THIS EXISTS
//
// Before this class, every failure path in runOta() was:
//
//     gs->setOtaStatus(OTA_FAILED); delay(3000); ESP.restart();
//
// and the SUCCESS path was `ESP.restart()`. From two feet away - which is where
// the operator is, because the lathe is between them and the screen - a failed
// update and a successful one are the same event: a progress bar, then a reboot.
// Three seconds of "Update failed" in 26 px is easy to walk past entirely, and
// "No update available" rebooted too, so even *nothing happening* looked the
// same. That cost a filming session: a thread-cutting fault was chased for hours
// partly because nobody could say whether the fix had been flashed or whether
// the lathe was still running the old, wrong firmware.
//
// So the requirement is not "report an error code". It is:
//
//   1. A failure must be IMPOSSIBLE TO CONFUSE with a success.
//   2. It must say WHY, in words that name the operator's next move (no Wi-Fi /
//      no server / stalled at 18% / bad image / already up to date).
//   3. It must NOT silently reboot into the old firmware as though the update
//      had happened. See exitAction(): RebootNow is reachable only from Success.
//   4. A success must confirm itself AFTER the reboot, because that is the only
//      moment the operator can trust - see OtaNotice / restore().
//
// THE HOLD-VS-ACKNOWLEDGE QUESTION (the design decision this class exists to make)
//
// The safe-looking answer is "a failure blocks on an operator keypress". The
// trouble is that this is a lathe: the update may have been started by someone
// who then walked away, and a modal that waits forever leaves the machine
// parked on a dialog with the spindle logic behind it. The unsafe-looking
// answer is the current one - three seconds and gone.
//
// What is implemented is the middle path, in three parts:
//
//   * ASYMMETRIC HOLDS. Success clears in kSuccessHoldMs (2 s): it is going to
//     prove itself after the reboot anyway, so dwelling on it just makes the
//     update feel slow. A failure holds kFailureHoldMs.
//   * ACKNOWLEDGEMENT SHORT-CIRCUITS THE HOLD. The hold exists only to stop the
//     outcome being missed; a keypress is proof it was not, so acknowledge()
//     releases immediately. Waiting out a timer someone is actively standing in
//     front of teaches them to ignore the screen.
//   * THE ACK IS BOUNDED, BUT THE EVIDENCE IS NOT. If nobody acknowledges a
//     failure within kAckTimeoutMs the modal releases on its own, so the
//     machine is never trapped - but noticePending() stays true, so the normal
//     screen keeps a "LAST UPDATE FAILED" banner until somebody clears it. The
//     dialog is transient; the fact is not.
//
// kFailureHoldMs (10 s) AND kAckTimeoutMs (300 s) ANSWER DIFFERENT QUESTIONS,
// even though nothing on screen distinguishes them - the modal looks identical
// whether it is 3 s past the floor or 3 s from the timeout, and that is exactly
// what makes the two easy to conflate.
//
//   * kFailureHoldMs is an UN-DISMISSABLE FLOOR. Its only job is to stop a
//     stray keypress clearing the notice before it has been readable at all.
//     Once the OK key was wired to acknowledge() (see UiFocus::Ota), an
//     operator standing right there who has already read "UPDATE FAILED -
//     <reason>" is not made to wait any longer than this for OK to do
//     something.
//   * kAckTimeoutMs is the OPERATOR-FACING UNATTENDED DWELL - the time the
//     machine sits parked on the modal if nobody presses anything at all. The
//     owner's own reasoning for 300 s: "I don't want to miss it if I'm away in
//     the workshop looking at something else." This is the number that was
//     ever the unattended wait; kFailureHoldMs never was one on its own, it is
//     only a lower bound on it.
//
// WHAT 300 S COSTS, spelled out rather than left to be rediscovered: while
// GlobalState::hasOTA() is true, timerCallback() (src/main.cpp) runs
// commsManager.loop() instead of spindle->update() + leadscrew->update(), so
// an unattended, unacknowledged failure leaves the motion loop idle for up to
// five minutes. Accepted, not overlooked - the operator just asked for the
// update themselves, so the machine is idle for that reason already, and the
// wired ack key means an attended operator is never actually waiting that
// long - but "the lathe was dead for five minutes" is exactly the kind of
// thing that reads as a hang to whoever meets it next without this context.
//
// AND NOTHING THAT FAILED REBOOTS. An ESP32 OTA writes the *inactive* partition,
// so a failed download has not touched the running image: there is nothing a
// restart can fix and everything it can hide. Rebooting on failure is precisely
// the behaviour that made a failure look like a success. Only Success reboots
// (it must, to run the new image), and UpToDate does not either - "already up to
// date" followed by a reboot is the most confusing sequence the old code had.
#ifndef ELS_OTA_OTAOUTCOME_H
#define ELS_OTA_OTAOUTCOME_H

#include <stdint.h>

// Where the attempt had got to. Reported by the caller as it goes; used to
// classify a stall (only a Downloading attempt can stall) and to drive the
// progress screen's wording.
enum class OtaPhase : uint8_t {
  Idle,         // nothing started yet
  Connecting,   // joining Wi-Fi
  Checking,     // asking GitHub for the latest tag_name
  Downloading,  // streaming the image into the OTA partition
  Finishing,    // Update.end() / verification
  Done,         // settled - result() says how
};

// The outcome, in the operator's terms rather than the HTTP layer's. Values are
// pinned (`: uint8_t`, explicit ordering) because they travel through OtaNotice
// into persistent storage and back after a reboot.
enum class OtaResult : uint8_t {
  InProgress = 0,       // not settled yet
  Success = 1,          // image written and verified; reboot to run it
  UpToDate = 2,         // the release tag equals FIRMWARE_VERSION; nothing done
  NoNetwork = 3,        // could not join Wi-Fi at all
  NoServer = 4,         // joined, but the server did not answer (TLS/HTTP/404)
  DownloadStalled = 5,  // the stream started and then stopped or ran short
  BadImage = 6,         // downloaded, but Update rejected it
};

// What the caller should do once the outcome has been on screen long enough.
enum class OtaExit : uint8_t {
  Waiting,          // keep the OTA screen up; do nothing
  RebootNow,        // ESP.restart() - reachable ONLY from Success
  ReturnToMachine,  // drop the OTA screen, go back to the normal UI, no reboot
};

// A settled outcome, flattened so it can survive the reboot that a successful
// update ends in. Trivially copyable POD by design: the caller blits it into RTC
// slow memory (or NVS) before ESP.restart() and hands it to restore() on the way
// up, which is how "UPDATED - now running 1.4.2" appears on the FIRST SCREEN OF
// THE NEW FIRMWARE. That banner is the only unambiguous evidence an update
// applied, because it is written by the code that only exists if it did.
struct OtaNotice {
  uint32_t magic;       // kNoticeMagic; anything else means "no notice stored"
  uint8_t result;       // an OtaResult, never InProgress
  uint8_t pad;          // keep the struct's layout explicit
  int16_t code;         // HTTP status or Update error, 0 when not applicable
  uint32_t bytesDone;   // download progress at the moment it settled
  uint32_t bytesTotal;  // 0 when the length was never known
  char version[24];     // release tag, or "" if the check never answered
};

class OtaOutcome {
 public:
  OtaOutcome();

  // --- Reporting, called by the OTA task as it goes -------------------------

  // A fresh attempt. Clears any previous outcome AND any pending notice: the
  // operator retrying is the strongest possible acknowledgement of the last
  // failure, and two stacked banners help nobody.
  void begin(unsigned long nowMs);

  // Where we are now. Passing Downloading (re)arms the stall watchdog, so it is
  // armed from the moment the transfer is meant to start, not from the first
  // byte - a transfer that never delivers a single byte is the commonest stall.
  void notePhase(OtaPhase phase, unsigned long nowMs);

  // Progress from Update.onProgress(). ONLY AN INCREASE COUNTS as progress: the
  // callback can fire repeatedly with an unchanged count, and treating that as
  // life would defeat the stall watchdog completely.
  void noteProgress(unsigned long bytesDone, unsigned long bytesTotal,
                    unsigned long nowMs);

  // The version check could not be answered (no network, GitHub 403, no
  // tag_name in the JSON). runOta() deliberately downloads anyway - the
  // operator asked for an update and the image is verified before it is kept -
  // but the outcome has to SAY the version was never confirmed, otherwise a
  // "Success" with no version number looks identical to one with a wrong one.
  void noteVersionUnknown();

  // The tag the server reported, if it did. Stored for both the UpToDate
  // message and the post-reboot success banner.
  void noteVersion(const char* tag);

  // Terminal transitions. Each is a one-way door: the first one to be called
  // wins and later calls are ignored, so an unwinding failure path cannot
  // overwrite a more specific diagnosis with a vaguer one.
  void succeed(unsigned long nowMs);
  void upToDate(unsigned long nowMs);
  // `code` is the HTTP status or Update error number, 0 if none. `note` is an
  // optional short library string (Update.errorString()); it is copied, never
  // held by pointer, because the caller's buffer does not outlive the reboot.
  void fail(OtaResult reason, unsigned long nowMs, int code = 0,
            const char* note = nullptr);

  // Backstop for the failure mode this project has actually measured: the
  // download that hard-stalls around 18% when Wi-Fi modem sleep is left on.
  // Returns true if it settled the outcome as DownloadStalled. Safe to poll.
  bool failIfStalled(unsigned long nowMs);

  // --- What happened --------------------------------------------------------

  OtaResult result() const { return m_result; }
  OtaPhase phase() const { return m_phase; }
  bool settled() const { return m_result != OtaResult::InProgress; }
  // The single question the display asks before choosing a colour. Anything
  // that is not Success and not UpToDate is a failure, including results added
  // later - the default must be "this went wrong", never "this was fine".
  bool failed() const;
  int code() const { return m_code; }
  bool versionKnown() const { return m_versionKnown; }
  const char* version() const { return m_version; }

  unsigned long bytesDone() const { return m_bytesDone; }
  unsigned long bytesTotal() const { return m_bytesTotal; }
  // 0..100, or -1 when the content length was never known. Integer maths only:
  // this is read from the display task and there is no reason to pull the FPU
  // into it.
  int percent() const;

  // No byte-count increase for kStallTimeoutMs while Downloading. False once
  // settled, and false in every other phase - a slow TLS handshake is the HTTP
  // layer's timeout to police, not this watchdog's.
  bool stalled(unsigned long nowMs) const;

  // --- How it must be presented ---------------------------------------------

  // Three words at most, and the three settled ones are deliberately unalike at
  // a glance rather than three variations on "update": UPDATED / UP TO DATE /
  // UPDATE FAILED.
  const char* headline() const { return m_headline; }

  // One line naming the operator's next move. Fits the OTA screen's 267 px
  // budget at Montserrat 26 only for the short ones - the display should render
  // it a size or two down; the point is that it is specific ("Stalled at 18% of
  // 1.5 MB"), not that it is large.
  const char* detail() const { return m_detail; }

  // Minimum time the settled outcome stays on screen. Asymmetric on purpose -
  // see the header comment.
  unsigned long holdMs() const;

  // Whether this outcome wants a keypress. True for failures only.
  bool requiresAck() const { return failed(); }

  // The operator pressed OK on the OTA screen. Cross-task (set on the
  // DisplayTask via ButtonPad, read by the OTA task), so volatile, single
  // writer, single reader - the same no-locks bargain as AlarmMonitor's
  // requestClear(). Harmless before the outcome settles; it is only ever read
  // through exitAction().
  void acknowledge() { m_acked = true; }
  bool acknowledged() const { return m_acked; }

  // The whole decision, in one call the OTA task can poll.
  OtaExit exitAction(unsigned long nowMs) const;

  // --- Evidence that outlives the screen ------------------------------------

  // Should the NORMAL screen carry a banner about this outcome? True for an
  // unacknowledged failure, indefinitely: a failure that timed out of its modal
  // has been seen by nobody, and the alternative is the machine quietly running
  // firmware the operator believes it replaced. For a Success restored after
  // the reboot it is true for kNoticeMs (10 s) and then clears itself - the
  // good news needs to be seen once, not carried around.
  bool noticePending(unsigned long nowMs) const;

  // Dismiss the banner without it counting as an acknowledgement of anything
  // else. acknowledge() also dismisses it.
  void clearNotice() { m_noticeCleared = true; }

  // Flatten / rehydrate across the reboot. snapshot() is only meaningful once
  // settled; noticeValid() is the guard for the garbage that unpowered RTC
  // memory holds on a cold boot. restore() puts the object straight into a
  // settled, Done state with the banner armed at nowMs, WITHOUT re-arming any
  // reboot: exitAction() on a restored outcome is ReturnToMachine once its hold
  // has elapsed, never RebootNow. Rebooting on the strength of a stored notice
  // would be a boot loop.
  OtaNotice snapshot() const;
  static bool noticeValid(const OtaNotice& n);
  void restore(const OtaNotice& n, unsigned long nowMs);

  // For Serial logs, so the log line and the screen cannot drift apart.
  static const char* resultName(OtaResult r);

  // --- Constants (all wall-clock ms) ----------------------------------------

  // Brisk: the reboot and the new version number are the real confirmation.
  static const unsigned long kSuccessHoldMs = 2000;
  // "Already up to date" is not a failure but it IS a result people miss, and
  // it is the one case where nothing else will ever tell them.
  static const unsigned long kInfoHoldMs = 6000;
  // THE UN-DISMISSABLE FLOOR, not the unattended dwell - see the header
  // comment ("kFailureHoldMs AND kAckTimeoutMs ANSWER DIFFERENT QUESTIONS").
  // Stops a stray keypress clearing the notice before it has been readable;
  // an attended operator's OK is honoured almost immediately above this.
  static const unsigned long kFailureHoldMs = 10000;
  // THE UNATTENDED SELF-RELEASE. After this the modal releases itself,
  // unacknowledged, and the banner takes over. The machine is never left
  // parked on a dialog nobody is coming to. 300 s (owner: "I don't want to
  // miss it if I'm away in the workshop looking at something else") - see the
  // header comment for what this costs the motion loop while it runs out.
  static const unsigned long kAckTimeoutMs = 300000;
  // How long a restored SUCCESS banner shows on the new firmware's first screen.
  static const unsigned long kNoticeMs = 10000;
  // No byte in this long during a download = stalled. Longer than the HTTPClient
  // read timeout (15 s) on purpose, so the HTTP layer gets first refusal and
  // this only catches what it misses. Short enough that the measured 18 %
  // modem-sleep hang is reported rather than waited out.
  static const unsigned long kStallTimeoutMs = 20000;

  static const uint32_t kNoticeMagic = 0x0A70UL << 16 | 0x0001UL;

  // Sizes of the two rendered strings. Small deliberately: they are members of
  // an object that lives on the OTA task's stack budget.
  static const int kHeadlineLen = 16;
  static const int kDetailLen = 48;
  static const int kVersionLen = 24;

 private:
  void render();  // rebuild m_headline / m_detail from the current state
  void settle(OtaResult r, unsigned long nowMs, int code, const char* note);

  OtaResult m_result;
  OtaPhase m_phase;
  int m_code;
  unsigned long m_bytesDone;
  unsigned long m_bytesTotal;
  unsigned long m_lastProgressMs;  // last byte-count INCREASE, or phase entry
  unsigned long m_settledAtMs;
  bool m_downloading;  // stall watchdog armed
  bool m_versionKnown;
  bool m_noticeCleared;
  bool m_restored;  // came back from a notice; must never ask for a reboot
  volatile bool m_acked;

  char m_version[kVersionLen];
  char m_note[kDetailLen];
  char m_headline[kHeadlineLen];
  char m_detail[kDetailLen];
};

#endif
