#include "DebugSink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <globalstate.h>
#include <string.h>

#include "WebSettings.h"
#include "version.h"

// The SpindleTask's handle, owned by main.cpp. The upload SUSPENDS it - see
// the long note in startUpload() for why that, and not a flag on the hot path.
extern TaskHandle_t spindleTaskHandle;

// Mozilla CA root bundle, for an https:// sink. Same core-version-specific
// symbol the OTA path uses (src/ESPCommsManager.cpp) - if the arduino-esp32
// core is upgraded, both need re-checking with `nm`.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

namespace {

// One upload at a time. Written by the DisplayTask (set) and the upload task
// (cleared); a single aligned bool, no lock, like everything else that crosses
// tasks here (CLAUDE.md).
volatile bool g_uploadRunning = false;

// Don't hammer a sink that is down. A failed upload keeps its trace, so it can
// simply be retried the next time the machine is at rest - but not ten times a
// second.
const uint32_t kRetryCooldownMs = 30000;
uint32_t g_nextAttemptMs = 0;

const uint32_t kWifiTimeoutMs = 20000;
const uint32_t kSocketTimeoutMs = 15000;

// Rows are batched into one buffer and sent as a single chunk, rather than a
// chunk (and a TCP write) per row. 1 KB holds about a dozen rows and costs
// nothing next to the 16 KB task stack.
const size_t kChunkBytes = 1024;

bool atRest(Leadscrew* leadscrew, GlobalState* gs) {
  const GlobalMotionMode motion = gs->getMotionMode();
  if (motion != GlobalMotionMode::MM_DISABLED &&
      motion != GlobalMotionMode::MM_UNSET) {
    return false;
  }
  // "Commanded to stop" is not "stopped": MM_DECELLERATE resolves to
  // MM_DISABLED only once the ramp reaches zero, and the planner's speed is
  // the authority on that (see getLeadscrewSpeedPulsesPerSecond()).
  return leadscrew->getLeadscrewSpeedPulsesPerSecond() == 0.0f;
}

// Reads the response status line far enough to know whether the sink accepted
// it. Anything that is not a 2xx counts as a failure, so the trace is kept and
// retried rather than thrown away on a 500.
bool readStatusOk(WiFiClient* client) {
  char line[64];
  size_t n = 0;
  // Signed difference against the deadline, so a millis() rollover mid-wait
  // cannot end the read early (or never).
  const uint32_t deadline = millis() + kSocketTimeoutMs;
  while ((int32_t)(millis() - deadline) < 0 && n < sizeof(line) - 1) {
    if (!client->available()) {
      if (!client->connected()) {
        break;
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    const int c = client->read();
    if (c < 0) {
      break;
    }
    if (c == '\n') {
      break;
    }
    if (c != '\r') {
      line[n++] = (char)c;
    }
  }
  line[n] = '\0';
  Serial.printf("capture: sink said \"%s\"\n", line);
  // "HTTP/1.1 200 OK"
  const char* space = strchr(line, ' ');
  return space != 0 && space[1] == '2';
}

bool sendChunk(WiFiClient* client, const char* data, size_t len) {
  if (len == 0) {
    return true;
  }
  char sizeLine[16];
  const int m = snprintf(sizeLine, sizeof(sizeLine), "%X\r\n", (unsigned)len);
  if (client->write((const uint8_t*)sizeLine, (size_t)m) != (size_t)m) {
    return false;
  }
  if (client->write((const uint8_t*)data, len) != len) {
    return false;
  }
  return client->write((const uint8_t*)"\r\n", 2) == 2;
}

// The upload proper. Returns true only when the sink answered 2xx.
bool postTrace(const HttpUrlParts& url, const DebugData* trace, int count) {
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  WiFiClient* client;
  if (url.secure) {
    secureClient.setCACertBundle(rootca_crt_bundle_start);
    client = &secureClient;
  } else {
    client = &plainClient;
  }
  client->setTimeout(kSocketTimeoutMs / 1000);

  if (!client->connect(url.host, (uint16_t)url.port)) {
    Serial.printf("capture: connect to %s:%d failed\n", url.host, url.port);
    return false;
  }

  // Chunked, not Content-Length: the body is generated a row at a time and its
  // exact byte count is not known in advance (%g output is variable width).
  // Building the whole ~215 KB body in RAM to measure it would need a second
  // allocation the size of the trace itself, next to the trace.
  String head = String("POST ") + url.path + " HTTP/1.1\r\n";
  head += String("Host: ") + url.host + "\r\n";
  head += "User-Agent: TeensyELS-capture\r\n";
  head += "Content-Type: text/csv\r\n";
  // Metadata the sink puts in the filename / summary line, so a capture can be
  // told apart from the next one without opening it.
  head += String("X-ELS-Version: ") + FIRMWARE_VERSION + "\r\n";
  head += String("X-ELS-Samples: ") + String(count) + "\r\n";
  head += String("X-ELS-Device: ") + WiFi.macAddress() + "\r\n";
  head += "Transfer-Encoding: chunked\r\n";
  head += "Connection: close\r\n\r\n";
  if (client->print(head) == 0) {
    client->stop();
    return false;
  }

  char chunk[kChunkBytes];
  size_t used = 0;

  // The header line first, then one row per sample.
  const char* csvHeader = debugCsvHeader();
  used = (size_t)snprintf(chunk, sizeof(chunk), "%s\n", csvHeader);

  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    char row[kDebugCsvRowMax];
    const int n = formatDebugRow(row, sizeof(row), trace[i]);
    if (n < 0) {
      continue;  // cannot happen: kDebugCsvRowMax is sized for any sample
    }
    if (used + (size_t)n + 1 >= sizeof(chunk)) {
      ok = sendChunk(client, chunk, used);
      used = 0;
    }
    memcpy(chunk + used, row, (size_t)n);
    used += (size_t)n;
    chunk[used++] = '\n';
  }
  if (ok) {
    ok = sendChunk(client, chunk, used);
  }
  // The terminating zero-length chunk. Without it the sink waits for more body
  // and the POST never completes.
  if (ok) {
    ok = client->write((const uint8_t*)"0\r\n\r\n", 5) == 5;
  }

  const bool accepted = ok && readStatusOk(client);
  client->stop();
  return accepted;
}

void uploadTask(void* arg) {
  (void)arg;  // the at-rest test was made by the poll, before we got here
  GlobalState* gs = GlobalState::getInstance();
  DebugCapture& dbg = gs->debug();
  bool sent = false;

  // SUSPEND THE SPINDLE TASK FOR THE DURATION.
  //
  // Not a flag tested inside the motion loop - the whole point of this feature
  // set is that nothing is added to that loop while it is not capturing, and a
  // "should I yield?" test there would be exactly that. Suspension costs the
  // hot path nothing at all, and it is the only way the WiFi driver (core 0,
  // priority 23) gets any CPU: the SpindleTask runs a non-blocking loop at
  // priority 24 on the same core and would starve it outright.
  //
  // Safe precisely because of the at-rest gate that got us here: no step is
  // due, and the planner's speed is zero. What is NOT safe is resuming into a
  // stale spindle delta - the encoder keeps counting while we are suspended,
  // so an axis left engaged would come back to a following error of thousands
  // of pulses and bolt for it. Hence the forced MM_DISABLED before the resume
  // below, which makes the first update() after resume pin m_expectedPosition
  // to the current position and discard the accumulated count.
  if (spindleTaskHandle != nullptr) {
    vTaskSuspend(spindleTaskHandle);
  }

  WebSettings* webSettings = getWebSettings();
  HttpUrlParts url;

  if (webSettings->debugUrl[0] == '\0') {
    // Nowhere to send. Recording still worked, so this is a configuration
    // problem, not a capture failure - it lands in DBG_FAILED like any other
    // unsuccessful send, and the trace is kept in case the URL is filled in.
    Serial.println("capture: no debug URL configured, not sending");
  } else if (!parseHttpUrl(webSettings->debugUrl, url)) {
    Serial.printf("capture: unusable debug URL \"%s\"\n", webSettings->debugUrl);
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(webSettings->ssid, webSettings->password);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < kWifiTimeoutMs) {
      vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("capture: WiFi connect timed out");
    } else {
      Serial.printf("capture: sending %d samples to %s:%d%s\n", dbg.count(),
                    url.host, url.port, url.path);
      sent = postTrace(url, dbg.data(), dbg.count());
    }
  }

  // Put the radio away again: it shares core 0 with the motion loop, and there
  // is no reason for it to stay associated once the trace is gone.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delete webSettings;

  if (sent) {
    // Hands the ~100 KB back but keeps the count, so the screen can still say
    // "SENT 2400". readyToSend() goes false with the buffer, so this cannot be
    // uploaded twice.
    dbg.release(DBG_SENT);
    Serial.println("capture: sent");
  } else {
    // The trace is KEPT. A failed send must not cost the cut that produced it;
    // the poll retries after the cooldown.
    dbg.setState(DBG_FAILED);
    Serial.println("capture: send FAILED, trace kept for retry");
  }

  // Disengage before resuming - see the suspension note above. The operator
  // has to press ENABLE again, which is the honest outcome: the machine has
  // been out of sync with the spindle for the whole upload.
  gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
  gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
  if (spindleTaskHandle != nullptr) {
    vTaskResume(spindleTaskHandle);
  }

  g_nextAttemptMs = millis() + kRetryCooldownMs;
  g_uploadRunning = false;
  vTaskDelete(nullptr);
}

}  // namespace

void debugCapturePoll(Leadscrew* leadscrew) {
  if (leadscrew == nullptr) {
    return;
  }
  GlobalState* gs = GlobalState::getInstance();

  // OTA owns the radio and is about to reboot the chip; stay out of its way.
  if (gs->hasOTA()) {
    return;
  }
  if (g_uploadRunning) {
    return;
  }

  DebugCapture& dbg = gs->debug();
  if (!dbg.readyToSend()) {
    return;
  }
  // Cooldown after a failure. millis() wraps every ~49 days; the signed
  // difference is the wrap-correct way to ask "has the deadline passed".
  if ((int32_t)(millis() - g_nextAttemptMs) < 0) {
    return;
  }
  if (!atRest(leadscrew, gs)) {
    return;  // the operator is still cutting - nothing to do but wait
  }

  // Published BEFORE the task exists, so the Diagnostics screen changes to
  // "SENDING TRACE" on this same display pass, and so a second pass cannot
  // start a second upload.
  dbg.setState(DBG_SENDING);
  g_uploadRunning = true;

  // Its own task, on core 1, with a 20 KB stack: an https:// sink puts a TLS
  // session and the CA bundle on it, which is what makes the OTA task 24 KB.
  // Never the SpindleTask (4 KB) - that is the mistake the OTA path documents.
  if (xTaskCreatePinnedToCore(uploadTask, "DbgSend", 20480, leadscrew, 1,
                              nullptr, 1) != pdPASS) {
    Serial.println("capture: could not start upload task");
    g_uploadRunning = false;
    dbg.setState(DBG_FAILED);
    g_nextAttemptMs = millis() + kRetryCooldownMs;
  }
}
