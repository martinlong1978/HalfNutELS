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

ESPCommsManager::ESPCommsManager(/* args */) {}

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
    Serial.print("OTA: WiFi connected, IP ");
    Serial.println(WiFi.localIP());
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
        Serial.println("OTA: http.begin(download) failed");
        return false;
    }
    http.setUserAgent(OTA_USER_AGENT);
    // Follow github.com -> *.githubusercontent.com asset redirect.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OTA: download HTTP %d\n", code);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.printf("OTA: bad content length %d\n", contentLength);
        http.end();
        return false;
    }

    // Prime the display progress bar and keep it fed as the stream is written.
    gs->setOTAContentLength(contentLength);
    gs->setOTABytes(0);
    Update.onProgress([](size_t done, size_t total) {
        GlobalState* g = GlobalState::getInstance();
        g->setOTAContentLength((int)total);
        g->setOTABytes((int)done);
    });

    if (!Update.begin(contentLength)) {
        Serial.printf("OTA: Update.begin failed: %s\n", Update.errorString());
        http.end();
        return false;
    }

    size_t written = Update.writeStream(http.getStream());
    http.end();

    if (written != (size_t)contentLength) {
        Serial.printf("OTA: wrote %u/%d bytes: %s\n", (unsigned)written,
                      contentLength, Update.errorString());
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        Serial.printf("OTA: Update.end failed: %s\n", Update.errorString());
        return false;
    }

    // Make sure the display shows a full bar before the reboot.
    gs->setOTABytes(contentLength);
    Serial.println("OTA: flash complete");
    return Update.isFinished();
}

void ESPCommsManager::otaTask(void* arg) {
    static_cast<ESPCommsManager*>(arg)->runOta();
    vTaskDelete(nullptr);  // runOta() restarts the chip; this is a safety net.
}

void ESPCommsManager::loop() {
    // Called repeatedly from the SpindleTask while hasOTA() is true. The SpindleTask
    // stack (4 KB) is far too small for the TLS handshake + CA-bundle validation +
    // Update, so spawn a dedicated big-stack task to do the work; it ends in
    // ESP.restart(). The guard makes this a one-shot.
    if (updating) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
        return;
    }
    updating = true;
    xTaskCreatePinnedToCore(otaTask, "OTA", 24576, this, 3, nullptr, 0);
}

void ESPCommsManager::runOta() {
    GlobalState* gs = GlobalState::getInstance();
    gs->setOtaStatus(OTA_CHECKING);

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
        gs->setOtaStatus(OTA_FAILED);
        delay(3000);
        ESP.restart();
        return;  // unreachable
    }

    // 2 + 3. GitHub version check.
    if (isGithub) {
        char tag[64] = {0};
        if (fetchLatestTag(GITHUB_API_URL, tag, sizeof(tag))) {
            Serial.printf("OTA: latest=%s current=%s\n", tag, FIRMWARE_VERSION);
            if (strcmp(tag, FIRMWARE_VERSION) == 0) {
                // Already up to date - show it, then reboot back into firmware.
                gs->setOtaStatus(OTA_NO_UPDATE);
                delay(3000);
                ESP.restart();
                return;  // unreachable
            }
        } else {
            // Couldn't determine the latest version. The user explicitly asked
            // for an update, so fall through to the download; the flash itself
            // is verified and will fail safe if the image is bad.
            Serial.println("OTA: version check failed, downloading anyway");
        }
    }

    // 4. Download + flash.
    gs->setOtaStatus(OTA_DOWNLOADING);
    if (downloadAndFlash(downloadUrl)) {
        // Success: boot into the new image.
        ESP.restart();
        return;  // unreachable
    }

    gs->setOtaStatus(OTA_FAILED);
    delay(3000);
    ESP.restart();
}
