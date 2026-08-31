#include "ESPCommsManager.h"
#include <globalstate.h>

// Mozilla CA root bundle embedded by arduino-esp32's esp_crt_bundle. The symbol
// name is core-version specific: this matches framework-arduinoespressif32
// 2.0.17 (the symbol linked from libmbedtls.a is `_binary_x509_crt_bundle_start`).
// In this core `setCACertBundle(NULL)` DETACHES the bundle, so we must pass this
// pointer explicitly to get server-certificate validation.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

// GitHub endpoints, derived from the repo constant in board.h. The download
// permalink always resolves to the newest release's asset named OTA_ASSET_NAME.
static const char* GITHUB_API_URL =
    "https://api.github.com/repos/" OTA_GITHUB_REPO "/releases/latest";
static const char* GITHUB_DOWNLOAD_URL =
    "https://github.com/" OTA_GITHUB_REPO "/releases/latest/download/" OTA_ASSET_NAME;

static const char* OTA_USER_AGENT = "HalfNutELS-OTA";

// The settled outcome of the update that has just rebooted the device, carried
// across the restart so the new firmware can say so.
//
// RTC_NOINIT_ATTR, NOT RTC_DATA_ATTR, for the reason main.cpp's bootToSetup
// spells out at length: .rtc.data is re-initialised from the image on a
// SOFTWARE restart, which is the only kind of restart this ever survives, so
// RTC_DATA_ATTR would zero it before setup() could read it. .rtc_noinit is not
// initialised on a cold boot either, which is what OtaOutcome::noticeValid()'s
// magic word is for - the garbage an unpowered RTC holds must not be mistaken
// for "your update worked".
//
// And NOT flash. Nothing here is worth a sector erase, but more importantly a
// flash write stops the opposite core and waits for it to acknowledge a
// cache-disable, and that core is core 1, which the spindle loop never yields
// (CLAUDE.md). RTC memory has none of that machinery.
static RTC_NOINIT_ATTR OtaNotice otaNotice;

ESPCommsManager* ESPCommsManager::s_active = nullptr;

ESPCommsManager::ESPCommsManager(/* args */) {
    // m_outcome initialises every one of its own members (otaoutcome.cpp), and
    // `updating` / m_lastProgressPublishMs both have in-class initialisers.
    // s_active is static, so it is the one thing here that is not per-object;
    // it is set when a run starts.
}

ESPCommsManager::~ESPCommsManager() {}

bool ESPCommsManager::wifiConnect(WebSettings* webSettings, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(webSettings->ssid, webSettings->password);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            Serial.println("OTA: WiFi connect timed out");
            return false;
        }
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    // Modem sleep is ON by default for WIFI_STA in arduino-esp32: between DTIM
    // beacons the radio powers down, so incoming data is only collected once per
    // DTIM period. For a request/response workload that is free power saving; for
    // a 1.5 MB bulk download it is throughput poison, because the TCP receive
    // window drains and refills once per beacon instead of continuously.
    //
    // That is not a guess about this device. The instrumented run measured one
    // ~1360-byte segment (a single MSS) arriving every ~350 ms, which is a beacon
    // period times a small DTIM count -- not what a busy or a lossy link looks
    // like. Turning sleep off for the update costs nothing: the download ends in
    // ESP.restart() either way.
    WiFi.setSleep(false);

    Serial.print("OTA: WiFi connected, IP ");
    Serial.println(WiFi.localIP());
    const int rssi = (int)WiFi.RSSI();
    Serial.printf("OTA: rssi=%d dBm, modem sleep %s\n", rssi,
                  WiFi.getSleep() ? "ON" : "OFF");
    // Fed to the screen, not just the log: a marginal reading is visible
    // during Connecting/Checking, before the download commits to it (see
    // OtaOutcome::signalDetail()'s header comment - raw dBm, not a
    // qualitative word, because CLAUDE.md records TLS connect itself failing
    // intermittently at the bottom of the measured -86..-49 dBm range).
    m_outcome.noteSignal(rssi);
    publishOutcome();
    return true;
}

bool ESPCommsManager::fetchLatestTag(const char* apiUrl, char* tagOut,
                                     size_t tagOutSize) {
    WiFiClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start);

    HTTPClient http;
    if (!http.begin(client, apiUrl)) {
        Serial.println("OTA: http.begin(API) failed");
        return false;
    }
    // GitHub 403s requests without a User-Agent; ask for the JSON API media type.
    http.setUserAgent(OTA_USER_AGENT);
    http.addHeader("Accept", "application/vnd.github+json");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OTA: version check HTTP %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Deliberately avoid an ArduinoJson dependency: pull tag_name out by hand.
    const char* key = "\"tag_name\":\"";
    int idx = payload.indexOf(key);
    if (idx < 0) {
        Serial.println("OTA: tag_name not found in release JSON");
        return false;
    }
    idx += strlen(key);
    int end = payload.indexOf('"', idx);
    if (end < 0) {
        return false;
    }
    String tag = payload.substring(idx, end);
    strncpy(tagOut, tag.c_str(), tagOutSize - 1);
    tagOut[tagOutSize - 1] = '\0';
    return true;
}

bool ESPCommsManager::downloadAndFlash(const char* url) {
    GlobalState* gs = GlobalState::getInstance();

    // Use TLS + CA bundle for https URLs (GitHub); plain client for the http://
    // home-network fallback (a secure client would fail the TLS handshake on :80).
    bool secure = strncmp(url, "https", 5) == 0;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    WiFiClient* client;
    if (secure) {
        secureClient.setCACertBundle(rootca_crt_bundle_start);
        client = &secureClient;
    } else {
        client = &plainClient;
    }

    HTTPClient http;
    if (!http.begin(*client, url)) {
        // Not a server error - the URL did not even parse into a connection.
        // Reported as NoServer with code 0, which renders "Server unreachable"
        // rather than inventing an HTTP status that never happened.
        Serial.println("OTA: http.begin(download) failed");
        m_outcome.fail(OtaResult::NoServer, millis(), 0, nullptr);
        return false;
    }
    http.setUserAgent(OTA_USER_AGENT);
    // Follow github.com -> *.githubusercontent.com asset redirect.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        // The code is carried onto the screen and into the log. It may be
        // NEGATIVE - HTTPClient returns its own errors that way (-1 connection
        // refused, -11 read timeout) - and that is deliberately not normalised:
        // "HTTP -11" and "HTTP 404" are different faults with different fixes,
        // and OtaNotice stores it as an int16_t precisely so both survive.
        Serial.printf("OTA: download HTTP %d\n", code);
        m_outcome.fail(OtaResult::NoServer, millis(), code, nullptr);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        // A 200 with no usable length. Still the server's fault, so NoServer,
        // but with no code: there is no status worth quoting.
        Serial.printf("OTA: bad content length %d\n", contentLength);
        m_outcome.fail(OtaResult::NoServer, millis(), 0, nullptr);
        http.end();
        return false;
    }

    // Prime the display progress bar and keep it fed as the stream is written.
    gs->setOTAContentLength(contentLength);
    gs->setOTABytes(0);
    m_outcome.noteProgress(0, (unsigned long)contentLength, millis());
    Update.onProgress([](size_t done, size_t total) {
        GlobalState* g = GlobalState::getInstance();
        g->setOTAContentLength((int)total);
        g->setOTABytes((int)done);
        const unsigned long now = millis();
        // The one write this callback makes that is NOT throttled below:
        // drawOTA() needs to know, with no delay, the instant this callback
        // stops firing (writeStream() blocking on a stalled connection), so
        // it can stop trusting the rate/ETA words already sitting in
        // m_otaDetail rather than wait for a throttle window to expire on a
        // publish that is never coming.
        g->setOtaProgressMs(now);
        // The callback is a bare function pointer with no `this`, so the
        // outcome is reached the same way GlobalState is: through a known
        // pointer rather than a capture. Null only if the callback somehow
        // outlives the run, which the `updating` one-shot prevents.
        ESPCommsManager* self = s_active;
        if (self != nullptr) {
            self->m_outcome.noteProgress((unsigned long)done, (unsigned long)total,
                                         now);
            // THROTTLED: this callback fires per chunk (hundreds of times
            // across a 1.5 MB transfer); see maybePublishProgress()'s comment
            // for why only the cross-task publish, not the string rebuild
            // inside OtaOutcome, needs gating.
            self->maybePublishProgress(now);
        }
    });

    if (!Update.begin(contentLength)) {
        // The partition would not open - wrong size, already busy, no room.
        // Nothing has been written, so the running image is untouched.
        Serial.printf("OTA: Update.begin failed: %s\n", Update.errorString());
        m_outcome.fail(OtaResult::BadImage, millis(), 0, Update.errorString());
        http.end();
        return false;
    }

    // Bulk transfer. Timed and reported because "the OTA download is slow" was
    // a real fault here once (see the modem-sleep note in wifiConnect()), and
    // the thing that made it decidable rather than arguable was a throughput
    // number. Three lines is cheap insurance against having to build that
    // instrument again from scratch.
    const uint32_t tStart = millis();
    size_t written = Update.writeStream(http.getStream());
    const uint32_t elapsedMs = millis() - tStart;
    http.end();

    const int postTransferRssi = (int)WiFi.RSSI();
    Serial.printf("OTA: %u bytes in %u ms (%u B/s), rssi=%d dBm\n",
                  (unsigned)written, (unsigned)elapsedMs,
                  (unsigned)(elapsedMs ? (uint32_t)((uint64_t)written * 1000 / elapsedMs) : 0),
                  postTransferRssi);
    // Not shown anywhere yet (the Downloading detail is bytes+rate+ETA, not
    // signal - see render()), but kept current for whatever reads
    // signalDbm()/signalDetail() next, same as the connect-time reading above.
    m_outcome.noteSignal(postTransferRssi);

    if (written != (size_t)contentLength) {
        // A short write is a stall, not a bad image: the bytes that DID arrive
        // were fine, they just stopped. Ask the watchdog first so the diagnosis
        // is its own if it has one - both land on DownloadStalled today, and
        // fail() is a one-way door, but the order says which authority spoke.
        //
        // failIfStalled() cannot be polled DURING the transfer from here:
        // writeStream() blocks until HTTPClient's own 15 s read timeout cuts it
        // short, and this is the first line that runs afterwards. That is what
        // the watchdog's 20 s is sized around (otaoutcome.h).
        Serial.printf("OTA: wrote %u/%d bytes: %s\n", (unsigned)written,
                      contentLength, Update.errorString());
        m_outcome.failIfStalled(millis());
        m_outcome.fail(OtaResult::DownloadStalled, millis(), 0, nullptr);
        Update.abort();
        return false;
    }

    // Every byte is in the partition; what is left is the verify. Worth its own
    // phase because it is the one part of an update where the screen would
    // otherwise sit at 100% saying "Downloading".
    m_outcome.notePhase(OtaPhase::Finishing, millis());
    publishOutcome();

    if (!Update.end(true)) {
        Serial.printf("OTA: Update.end failed: %s\n", Update.errorString());
        m_outcome.fail(OtaResult::BadImage, millis(), 0, Update.errorString());
        return false;
    }

    // Make sure the display shows a full bar before the reboot.
    gs->setOTABytes(contentLength);
    if (!Update.isFinished()) {
        // end(true) said yes and isFinished() says no. It should not happen; if
        // it does, the safe reading is that the image is not trustworthy - and
        // the old code returned this bool straight into a reboot.
        Serial.println("OTA: Update.end returned true but isFinished() is false");
        m_outcome.fail(OtaResult::BadImage, millis(), 0, "verify incomplete");
        return false;
    }

    Serial.println("OTA: flash complete");
    m_outcome.succeed(millis());
    return true;
}

void ESPCommsManager::otaTask(void* arg) {
    static_cast<ESPCommsManager*>(arg)->runOta();
    // runOta() returns on every path that is not a verified success (which
    // restarts the chip instead), so this is the normal way out now, not a
    // safety net: the task must delete itself or FreeRTOS traps on the return.
    vTaskDelete(nullptr);
}

void ESPCommsManager::loop() {
    // Called repeatedly from the SpindleTask while hasOTA() is true. The SpindleTask
    // stack (4 KB) is far too small for the TLS handshake + CA-bundle validation +
    // Update, so spawn a dedicated big-stack task to do the work. The guard makes
    // this a one-shot.
    //
    // That task no longer always ends in ESP.restart(): only a verified success
    // reboots. Anything else clears hasOTA() and releases this guard, which is
    // what makes a RETRY possible - the operator can ask for the update again
    // without power-cycling the lathe, and this call spawns a fresh task.
    if (updating) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
        return;
    }
    updating = true;
    xTaskCreatePinnedToCore(otaTask, "OTA", 24576, this, 3, nullptr, 0);
}

void ESPCommsManager::publishOutcome() {
    GlobalState* gs = GlobalState::getInstance();

    // Words first, then shape. Both tasks are on core 0 and no lock is taken
    // (CLAUDE.md), so a display frame can in principle land between these two
    // writes; this order makes the stale frame the harmless one - the previous
    // colour under the new text, rather than a red UPDATE FAILED colour applied
    // to the text of whatever was happening before it.
    gs->setOtaText(m_outcome.headline(), m_outcome.detail());
    gs->setOtaContextLine(m_outcome.contextLine());

    switch (m_outcome.result()) {
    case OtaResult::InProgress:
        gs->setOtaStatus((m_outcome.phase() == OtaPhase::Downloading ||
                          m_outcome.phase() == OtaPhase::Finishing)
                             ? OTA_DOWNLOADING
                             : OTA_CHECKING);
        break;
    case OtaResult::Success:
        gs->setOtaStatus(OTA_SUCCESS);
        break;
    case OtaResult::UpToDate:
        gs->setOtaStatus(OTA_NO_UPDATE);
        break;
    default:
        // Everything else is a failure, by exclusion - the same default
        // OtaOutcome::failed() takes, and for the same reason: a result added
        // later must read as "this went wrong", never as "this was fine".
        gs->setOtaStatus(OTA_FAILED);
        break;
    }
}

void ESPCommsManager::maybePublishProgress(unsigned long nowMs) {
    if (nowMs - m_lastProgressPublishMs < 250) {
        return;
    }
    m_lastProgressPublishMs = nowMs;
    publishOutcome();
}

bool ESPCommsManager::beginBootNotice() {
    const bool valid = OtaOutcome::noticeValid(otaNotice);
    // Cleared either way, and BEFORE anything can reboot again: the notice is
    // shown exactly once. Leaving a valid magic in RTC memory would put
    // "UPDATED" on the screen at every power-on until the next update.
    otaNotice.magic = 0;
    if (!valid) {
        return false;
    }

    m_outcome.restore(otaNotice, millis());
    // Borrow the OTA screen to show it. restore() marks the outcome as restored,
    // so exitAction() can only ever return ReturnToMachine from here - a stored
    // notice can never ask for another reboot, which would be a boot loop.
    //
    // setOTA() BEFORE publishOutcome(), always: setOTA() clears the bus's OTA
    // text so a retry cannot inherit the last attempt's words, and it would
    // clear these on their way out if the two were the other way round.
    GlobalState::getInstance()->setOTA();
    publishOutcome();
    Serial.printf("OTA: previous attempt %s - %s\n",
                  OtaOutcome::resultName(m_outcome.result()),
                  m_outcome.detail());
    return true;
}

bool ESPCommsManager::bootNoticeDone() {
    if (m_outcome.exitAction(millis()) == OtaExit::Waiting) {
        return false;
    }
    // Hand the screen back. Nothing else to undo: no task was started, no
    // socket opened, and `updating` was never set, so an update requested later
    // in this boot still spawns a fresh OTA task.
    GlobalState::getInstance()->clearOTA();
    return true;
}

void ESPCommsManager::runOta() {
    GlobalState* gs = GlobalState::getInstance();

    // Everything from here reports into m_outcome, and the single exit loop at
    // the bottom of this function is the ONLY place that reboots or gives the
    // screen back. There is deliberately no `delay(3000); ESP.restart();` left
    // anywhere: see the class comment.
    s_active = this;
    m_lastProgressPublishMs = 0;
    m_outcome.begin(millis());  // phase = Connecting
    // The version this device is running BEFORE the attempt - the other half
    // of the "v1.0.5 -> v1.0.6" transition line, once noteVersion() supplies
    // the release tag during the Checking phase below.
    // FIRMWARE_VERSION_ABOUT, not FIRMWARE_VERSION: the same identity the
    // About screen shows, which is the SHA on a build that is not a release.
    // A bench build takes its FIRMWARE_VERSION from the NEAREST TAG, so
    // passing the raw version made the screen read "v1.0.6 -> v1.0.6" while
    // downloading the real v1.0.6 over an unofficial build of the same commit.
    // "c7a4cd1* -> v1.0.6" is what actually happened.
    m_outcome.noteCurrentVersion(FIRMWARE_VERSION_ABOUT);
    publishOutcome();

    WebSettings* webSettings = getWebSettings();

    // Decide the source. Empty URL or a github.com URL -> treat as GitHub and do
    // the version-skip check. Any other URL is the home-network fallback: skip
    // the check and download it directly.
    const char* configuredUrl = webSettings->url;
    bool urlEmpty = (configuredUrl[0] == '\0');
    bool isGithub = urlEmpty || (strstr(configuredUrl, "github.com") != nullptr);
    const char* downloadUrl = urlEmpty ? GITHUB_DOWNLOAD_URL : configuredUrl;

    // 1. Connect WiFi (with timeout).
    if (!wifiConnect(webSettings, 20000)) {
        m_outcome.fail(OtaResult::NoNetwork, millis(), 0, nullptr);
    }

    // 2 + 3. GitHub version check.
    if (!m_outcome.settled() && isGithub) {
        m_outcome.notePhase(OtaPhase::Checking, millis());
        publishOutcome();

        char tag[64] = {0};
        if (fetchLatestTag(GITHUB_API_URL, tag, sizeof(tag))) {
            Serial.printf("OTA: latest=%s current=%s\n", tag, FIRMWARE_VERSION);
            m_outcome.noteVersion(tag);
            // ELS_BUILD_IS_RELEASE gates this, and it is not belt and
            // braces. A bench build takes its FIRMWARE_VERSION from the
            // NEAREST TAG, so a USB-flashed build of the v1.0.6 commit
            // reports "v1.0.6" and would compare equal to the published
            // v1.0.6 - leaving the machine sitting on an unofficial image
            // and told there was nothing to fetch, with no way to get the
            // real one short of another USB flash.
            //
            // A local build is never the artifact that was published, even
            // when the commit matches, so it is never up to date with
            // respect to a release. It is always offered the real thing.
            if (ELS_BUILD_IS_RELEASE && strcmp(tag, FIRMWARE_VERSION) == 0) {
                // Already up to date. This NO LONGER REBOOTS: "No update
                // available" followed by a restart was the single most
                // confusing sequence the old code had - nothing happened, and
                // the machine reacted as though something had.
                m_outcome.upToDate(millis());
            }
        } else {
            // Couldn't determine the latest version. The user explicitly asked
            // for an update, so fall through to the download; the flash itself
            // is verified and will fail safe if the image is bad. But the
            // outcome has to SAY the version was never confirmed, or a success
            // with no version reads the same as one with the wrong version.
            m_outcome.noteVersionUnknown();
            Serial.println("OTA: version check failed, downloading anyway");
        }
    } else if (!m_outcome.settled()) {
        // The home-network fallback URL. There is no tag to ask for, so the
        // version is genuinely unknown rather than unchecked-by-accident.
        m_outcome.noteVersionUnknown();
    }

    // 4. Download + flash. downloadAndFlash() settles the outcome itself, with
    // the specific reason, on every path it can take.
    if (!m_outcome.settled()) {
        m_outcome.notePhase(OtaPhase::Downloading, millis());
        publishOutcome();
        downloadAndFlash(downloadUrl);
    }

    // Belt and braces: the sequence above cannot fall through unsettled, but if
    // a future edit adds a path that does, the exit loop below would spin
    // forever on Waiting with the machine frozen behind the OTA screen.
    if (!m_outcome.settled()) {
        m_outcome.fail(OtaResult::BadImage, millis(), 0, "no result");
    }

    publishOutcome();
    Serial.printf("OTA: %s - %s (%s, code %d)\n", m_outcome.headline(),
                  m_outcome.detail(), OtaOutcome::resultName(m_outcome.result()),
                  m_outcome.code());

    // THE ONE EXIT. Every way this function can end now comes through here, and
    // the decision is OtaOutcome's, not this file's:
    //
    //   Waiting         - the outcome has not been on screen long enough yet.
    //   RebootNow       - reachable ONLY from Success. Snapshot into RTC memory
    //                     first, so the new image can confirm itself.
    //   ReturnToMachine - drop the OTA screen and go back to the lathe. NO
    //                     REBOOT: an ESP32 OTA writes the INACTIVE partition,
    //                     so a failed download has not touched the running
    //                     image. There is nothing a restart can fix and
    //                     everything it can hide.
    //
    // The 100 ms poll is slower than anything it is waiting on and keeps this
    // task off the CPU for the hold - up to 30 s on a failure.
    for (;;) {
        const OtaExit action = m_outcome.exitAction(millis());
        if (action == OtaExit::RebootNow) {
            otaNotice = m_outcome.snapshot();
            Serial.println("OTA: restarting into the new image");
            Serial.flush();
            ESP.restart();
            return;  // unreachable
        }
        if (action == OtaExit::ReturnToMachine) {
            break;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Give the machine back. Order matters: clearOTA() first, so the SpindleTask
    // stops calling loop() before the one-shot guard is released - the other way
    // round it would see hasOTA() still true with updating false and spawn a
    // second OTA task on the spot.
    gs->clearOTA();
    s_active = nullptr;
    updating = false;
}
