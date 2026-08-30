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

// What the SpindleTask is dropped to for the duration of an upload. Low
// enough to be out of the way of the WiFi driver (23) and the flash IPC task
// (24) on core 0; still a real priority, so the loop keeps running.
const UBaseType_t kUploadSpindlePriority = 2;

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
  head += "User-Agent: HalfNutELS-capture\r\n";
  head += "Content-Type: text/csv\r\n";
  // Metadata the sink puts in the filename / summary line, so a capture can be
  // told apart from the next one without opening it.
  // Full provenance, not the bare version: a capture uploaded from a demo or
  // dirty build must be attributable to it (issue #4).
  head += String("X-ELS-Version: ") + FIRMWARE_VERSION_DISPLAY + "\r\n";
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
  bool lowered = false;
  UBaseType_t spindlePriority = 0;

  // ORDERING RULE FOR THIS WHOLE FUNCTION: NO FLASH ACCESS WHILE THE SPINDLE
  // TASK IS SUSPENDED.
  //
  // A flash operation has to disable the flash cache on BOTH cores, which
  // means stopping the other core and waiting for it to acknowledge. Doing
  // that with the SpindleTask suspended deadlocked the device outright: the
  // upload task sat inside getWebSettings()'s ESP.flashRead forever, the
  // carriage was dead because the spindle task never came back, and nothing
  // recovered it - disableLoopWDT() plus the two esp_task_wdt_delete() calls
  // in main.cpp mean no watchdog is watching. Entirely silent: the screen just
  // said SENDING TRACE, permanently. Only a numbered trace through this
  // function found it, because "send failed" is what the operator saw and the
  // failure is not in sending.
  //
  // So the settings are read FIRST, at full speed, before anything is
  // suspended - nothing about reading them needs the motion loop stopped.
  Serial.println("capture: [0] upload task entered");
  WebSettings* webSettings = getWebSettings();
  Serial.println("capture: [1] settings read");
  HttpUrlParts url;
  bool haveUrl = false;

  if (webSettings->debugUrl[0] == '\0') {
    // Nowhere to send. Recording still worked, so this is a configuration
    // problem, not a capture failure - it lands in DBG_FAILED like any other
    // unsuccessful send, and the trace is kept in case the URL is filled in.
    Serial.println("capture: no debug URL configured, not sending");
  } else if (!parseHttpUrl(webSettings->debugUrl, url)) {
    Serial.printf("capture: unusable debug URL \"%s\"\n", webSettings->debugUrl);
  } else {
    haveUrl = true;
  }

  if (haveUrl) {
    // YIELD CORE 0 BY LOWERING THE SPINDLE TASK, NOT BY SUSPENDING IT.
    //
    // Two constraints that look circular until you pick the right lever:
    //  - WiFi cannot associate unless core 0 is available. The SpindleTask
    //    runs a non-blocking loop at priority 24 and the WiFi driver tasks sit
    //    at 23 on the same core, so they never get a cycle.
    //  - Flash operations (and WiFi bring-up performs several, in NVS) need
    //    core 0 to ACKNOWLEDGE a cache-disable request. A suspended task
    //    cannot, and the whole device deadlocks silently - see the ordering
    //    note above, which is what that cost us.
    //
    // Dropping the priority satisfies both: core 0 is free for the driver
    // tasks and for the flash IPC, while the spindle task still EXISTS and
    // still runs, so nothing that needs it to respond is left waiting. It also
    // removes the suspend/resume hazards entirely - there is no window in
    // which the loop is frozen mid-iteration.
    //
    // MM_DISABLED goes first, deliberately. The loop keeps running at low
    // priority, so the axis must be told not to chase the spindle before it is
    // slowed down: the encoder keeps counting throughout, and update() pins
    // m_expectedPosition to the current position while disabled rather than
    // accumulating a following error it would later bolt to close.
    gs->setMotionMode(GlobalMotionMode::MM_DISABLED);
    gs->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    if (spindleTaskHandle != nullptr) {
      spindlePriority = uxTaskPriorityGet(spindleTaskHandle);
      vTaskPrioritySet(spindleTaskHandle, kUploadSpindlePriority);
      lowered = true;
    }
    Serial.println("capture: [2] spindle yielded, waiting for WiFi");

    // Now the radio, with core 0 free for it. esp_wifi_init reads calibration
    // and configuration out of NVS, and persistent mode WRITES the credentials
    // back on every begin() - all flash operations, which is precisely why the
    // step above lowers the spindle task rather than suspending it. Under a
    // suspend these would have deadlocked exactly as the settings read did;
    // ahead of it they would simply have starved, which is what they did.
    //
    // persistent(false) is not just about that: these credentials came from our
    // own settings blob a few lines above, so re-persisting them to NVS on
    // every capture upload is a flash write that buys nothing.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(webSettings->ssid, webSettings->password);
    Serial.println("capture: [3] WiFi.begin returned, associating");
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < kWifiTimeoutMs) {
      vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("capture: WiFi connect timed out (wl_status=%d)\n",
                    (int)WiFi.status());
    } else {
      Serial.printf("capture: sending %d samples to %s:%d%s\n", dbg.count(),
                    url.host, url.port, url.path);
      sent = postTrace(url, dbg.data(), dbg.count());
    }

    // Put the radio away again: it shares core 0 with the motion loop, and
    // there is no reason for it to stay associated once the trace is gone.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  delete webSettings;

  if (sent) {
    // Hands the RAM back but keeps the count, so the screen can still say
    // "SENT 1000". readyToSend() goes false with the buffer, so this cannot be
    // uploaded twice.
    dbg.release(DBG_SENT);
    Serial.println("capture: sent");
  } else {
    // The trace is KEPT. A failed send must not cost the cut that produced it;
    // the poll retries after the cooldown.
    dbg.setState(DBG_FAILED);
    Serial.println("capture: send FAILED, trace kept for retry");
  }

  // Give the motion loop its priority back. MM_DISABLED was already set on the
  // way in and is deliberately NOT undone: the machine has been out of sync
  // with the spindle for the whole upload, so the operator pressing ENABLE
  // again is the honest outcome.
  if (lowered) {
    vTaskPrioritySet(spindleTaskHandle, spindlePriority);
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

  // Heap at the moment of truth. The trace is resident by now, and this is the
  // budget the whole upload - task stack, WiFi, sockets - has to fit inside.
  // Printed unconditionally: "could not start upload task" with no number
  // attached is exactly why this failure survived a bench session undiagnosed.
  Serial.printf("capture: heap free=%u largest=%u, trace=%u bytes\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)(dbg.count() * sizeof(DebugData)));

  // Hand back the block arm() set aside for exactly this moment. Until now it
  // has been held precisely so that nothing else could take it, and so that a
  // trace is never recorded that cannot afterwards be sent.
  dbg.releaseUploadReserve();

  // Its own task, on core 1, with a 20 KB stack: an https:// sink puts a TLS
  // session and the CA bundle on it, which is what makes the OTA task 24 KB.
  // Never the SpindleTask (4 KB) - that is the mistake the OTA path documents.
  if (xTaskCreatePinnedToCore(uploadTask, "DbgSend", 20480, leadscrew, 1,
                              nullptr, 0) != pdPASS) {
    Serial.println("capture: could not start upload task");
    g_uploadRunning = false;
    dbg.setState(DBG_FAILED);
    g_nextAttemptMs = millis() + kRetryCooldownMs;
  }
}
