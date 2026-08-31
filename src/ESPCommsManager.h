#ifndef ESPCommsManager_h
#define ESPCommsManager_h
#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <otaoutcome.h>
#include "config.h"
#include "WebSettings.h"
#include "version.h"

// Runs the over-the-air firmware update. Triggered by GlobalState::setOTA()
// (holding Half-Nut); loop() runs the whole sequence once, on its own task.
//
// IT NO LONGER "ALWAYS ENDS IN ESP.restart()", and that is the point of the
// OtaOutcome member below. It used to: every failure was
// `setOtaStatus(OTA_FAILED); delay(3000); ESP.restart();` and success was a
// bare ESP.restart(), so from where the operator stands - the far side of the
// lathe - a failed update and a successful one were the same event, a progress
// bar followed by a reboot, and the machine came quietly back up on the OLD
// firmware. That cost a filming session. lib/ota/otaoutcome.h carries the full
// reasoning; this class supplies it with facts and obeys its exitAction().
//
// The division of labour is deliberate and worth keeping: OtaOutcome is pure
// C++ and decides everything (what went wrong, how long it stays on screen,
// whether a reboot is allowed at all, what the words are); this class owns
// every side effect (sockets, flash, RTC memory, ESP.restart()) and none of the
// policy.
class ESPCommsManager {
private:
    bool updating = false;

    // How this attempt is going, and how it must be presented. Reported into as
    // the sequence runs, then polled by the single exit loop at the end of
    // runOta(). Lives here rather than on the OTA task's stack so the progress
    // callback can find it (see s_active) and so a restored boot notice has
    // somewhere to be restored INTO before any task exists.
    OtaOutcome m_outcome;

    // Update.onProgress() takes a plain C function pointer, so the callback has
    // no `this`. It already reaches GlobalState through its singleton; this is
    // the same trick for the outcome. Set at the top of runOta(), which is the
    // only place that can be running when the callback fires (there is exactly
    // one OTA task, guarded by `updating`).
    static ESPCommsManager* s_active;

    // Last millis() a progress-callback publish actually crossed into
    // GlobalState - see maybePublishProgress()'s comment for why this is
    // throttled at all.
    unsigned long m_lastProgressPublishMs = 0;

    // Connect to the configured WiFi network (STA). Returns false if it can't
    // associate within timeoutMs.
    bool wifiConnect(WebSettings* webSettings, uint32_t timeoutMs);

    // GET the GitHub "releases/latest" API and copy the release's tag_name into
    // tagOut. Returns false on any HTTP/parse error.
    bool fetchLatestTag(const char* apiUrl, char* tagOut, size_t tagOutSize);

    // Download the firmware image at url and write it to the OTA partition,
    // updating GlobalState progress as it streams. Returns true only on a fully
    // verified, finished flash - and, either way, SETTLES m_outcome with a
    // specific diagnosis rather than the bare bool the caller used to get.
    bool downloadAndFlash(const char* url);

    // Mirror m_outcome onto the coordination bus for the DisplayTask: the words
    // from headline()/detail(), the screen shape from result()/phase(). The one
    // place the two are translated, so the panel and the Serial log cannot
    // disagree about the same attempt.
    void publishOutcome();

    // Called from Update.onProgress() (via s_active, see that member) on
    // every chunk - hundreds of times across a 1.5 MB transfer. Throttled to
    // roughly every 250 ms: the string FORMATTING inside OtaOutcome::render()
    // happens on every noteProgress() call regardless (cheap - it is just
    // this object rewriting its own buffers) and is not what needs
    // throttling; what does is the cross-task publish into GlobalState this
    // method gates, which is the only part with a real per-call cost.
    void maybePublishProgress(unsigned long nowMs);

    // The whole update sequence (check -> download -> flash -> hold -> restart
    // or return). Runs on its own task because TLS + CA-bundle validation +
    // Update need far more stack than the SpindleTask (which calls loop()) has.
    void runOta();
    static void otaTask(void* arg);

public:
    ESPCommsManager();
    ~ESPCommsManager();
    void loop();

    // --- Post-reboot confirmation ------------------------------------------
    //
    // The half of this feature that cannot be done before the restart. A
    // successful update ends in ESP.restart(), and everything the OTA task knew
    // dies with it; the operator is left watching a device reboot, which is
    // also what a failure and a crash look like. So the outcome is blitted into
    // RTC_NOINIT memory (NOT flash - no wear, no cache-disable IPC against the
    // spindle core, and the same lifetime bootToSetup uses in main.cpp) and
    // picked up on the way back up.
    //
    // "UPDATED - now running v1.4.3", drawn by code that only exists in the new
    // image, is the only unambiguous proof an update applied.
    //
    // Call beginBootNotice() from setup() ABOVE the SpindleTask creation; if it
    // returns true, pump display->update() until bootNoticeDone() says the hold
    // has elapsed. Blocking there is safe for exactly the reason the splash
    // delay is (see main.cpp). Both are no-ops on an ordinary boot.
    bool beginBootNotice();
    bool bootNoticeDone();

    // OK on the OTA screen (UiIntent::AckOta, src/buttonpad.cpp). Forwards to
    // OtaOutcome::acknowledge() - documented safe to call at any phase of the
    // attempt, including before it has settled, which is why applyIntent()
    // does not need to ask requiresAck() first. m_outcome is private so this
    // is the one crossing point; acknowledge() itself is the volatile
    // single-writer (DisplayTask) / single-reader (OTA task) flag, the same
    // no-locks bargain as AlarmMonitor::requestClear().
    void acknowledgeOutcome() { m_outcome.acknowledge(); }
};

// The one ESPCommsManager, built by main.cpp (main.cpp:39). Reached by extern
// rather than injected for the same reason `extern Display* display;` is in
// src/buttonpad.cpp: ButtonPad is constructed before commsManager exists, so
// there is no ordering in which it could arrive through a constructor
// argument. Safe to call acknowledgeOutcome() through this from the
// DisplayTask regardless of what commsManager.loop() is doing on its own
// task - see the method's own comment.
extern ESPCommsManager commsManager;

#endif
