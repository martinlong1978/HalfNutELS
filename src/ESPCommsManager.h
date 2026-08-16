#ifndef ESPCommsManager_h
#define ESPCommsManager_h
#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"
#include "WebSettings.h"
#include "version.h"

// Runs the over-the-air firmware update. Triggered by GlobalState::setOTA()
// (holding Half-Nut). loop() runs the whole update sequence once and always
// ends in ESP.restart(), so it never has to tear down the LVGL OTA screen.
class ESPCommsManager {
private:
    bool updating = false;

    // Connect to the configured WiFi network (STA). Returns false if it can't
    // associate within timeoutMs.
    bool wifiConnect(WebSettings* webSettings, uint32_t timeoutMs);

    // GET the GitHub "releases/latest" API and copy the release's tag_name into
    // tagOut. Returns false on any HTTP/parse error.
    bool fetchLatestTag(const char* apiUrl, char* tagOut, size_t tagOutSize);

    // Download the firmware image at url and write it to the OTA partition,
    // updating GlobalState progress as it streams. Returns true only on a fully
    // verified, finished flash.
    bool downloadAndFlash(const char* url);

    // The whole update sequence (check -> download -> flash -> restart). Runs on
    // its own task because TLS + CA-bundle validation + Update need far more
    // stack than the SpindleTask (which calls loop()) has.
    void runOta();
    static void otaTask(void* arg);

public:
    ESPCommsManager();
    ~ESPCommsManager();
    void loop();
};

#endif
