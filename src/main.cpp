// Libraries
#include <Arduino.h>
#include <SPI.h>
//#include <Wire.h>
#include <globalstate.h>
#include <leadscrew.h>
#include <spindle.h>

#include "WebSettings.h"

#include "ESPCommsManager.h"
#include <WiFi.h>

#include "buttonpad.h"
#include "config.h"
#include "display.h"
#include "keyarray.h"
#include "setupmode.h"

//#define FULLMONITOR
#include <esp_task_wdt.h>
#include <leadscrew_io_esp.h>
#include "latheconfig.h"


Leadscrew* leadscrew;

KeyArray* keyArray;
ButtonPad* keyPad;
Display* display;
Spindle* spindle;

GlobalState* globalState = GlobalState::getInstance();

LeadscrewIOESP leadscrewIOImpl;

ESPCommsManager commsManager;


int64_t lastcycle;
int cyclecount;
int finalcyclecount;
bool configMode = false;
bool apClientConnected = false;

// Runtime path into config/AP mode (see docs/ux-redesign.md section 6).
//
// This can't be a flash-backed flag: WebSettings and LatheConfig share one 4 KB
// sector that is erased as a unit (see WebSettings.cpp), so a boot-flag write there
// would risk the stored Wi-Fi credentials and burn a flash erase/write cycle on
// every normal boot. RTC slow memory is the right lifetime instead, and it costs
// no flash wear. Do NOT change this to a flash write.
//
// IT MUST BE RTC_NOINIT_ATTR, NOT RTC_DATA_ATTR. The framework's own comments in
// esp_attr.h draw the distinction, and it is exactly the one that matters here:
//
//   RTC_DATA_ATTR   "will keep its value during a deep sleep / wake cycle"
//   RTC_NOINIT_ATTR "will keep its value AFTER RESTART or during a deep sleep"
//
// .rtc.data is re-initialised from the image on a software restart, so with
// RTC_DATA_ATTR this flag was zeroed by the startup code before setup() could
// read it: requestSetupOnNextBoot() rebooted the device and it came up normally
// every time. Holding OK at power-on still worked, because that path reads the
// keypad and never looks at this flag - which is what made the bug look like a
// menu problem rather than a storage one.
//
// .rtc_noinit is not initialised on a COLD boot either, so the flag holds
// whatever the RTC memory happened to contain. That is why the value compared
// against is a distinctive magic rather than a plain 0/1 - 0 and 0xFFFFFFFF are
// the two patterns most likely to appear as uninitialised RTC garbage and would
// false-trigger a setup boot. Keep the magic; it is load-bearing under NOINIT.
#define BOOT_TO_SETUP_MAGIC 0xE15B0071u
RTC_NOINIT_ATTR uint32_t bootToSetup;

void requestSetupOnNextBoot() {
  bootToSetup = BOOT_TO_SETUP_MAGIC;
  ESP.restart();
}


// have to handle the leadscrew updates in a timer callback so we can update the
// screen independently without losing pulses
void timerCallback() {
  if (GlobalState::getInstance()->hasOTA()) {
    commsManager.loop();
  } else {
    spindle->update();
    leadscrew->update();
  }
}


void displayLoop() {
  keyPad->handle();

  display->update();
}

void DisplayTask(void* parameter) {
  // Ensure interrupts are initialised on the right core.  
  keyArray->initPad();
  uint64_t m = 1;
  while (true) {
    displayLoop();
    esp_task_wdt_reset();
    //uint64_t c = micros();
    //uint64_t delay = (100000 - (c - m)) / 1000;
    //if (delay > 0) {
      //vTaskDelay((delay > 100 ? 100 : delay) / portTICK_PERIOD_MS);
    vTaskDelay((100) / portTICK_PERIOD_MS);
    //}
    //m = c + 100000;
  }
}

void SpindleTask(void* parameter) {
  while (true) {
    timerCallback();
    esp_task_wdt_reset();
  }
}

void comms_loop(void* parameters) { commsManager.loop(); }


const char* ssid = "ELS_Wifi";
const char* password = "123456789";

//WiFiServer server;

void runWifiSettings() {
  configMode = true;
  WiFi.mode(WIFI_AP);
  delay(100);
  bool result = WiFi.softAP(ssid, password);
  if (result == true) {
    Serial.println("Access Point Ready");
    Serial.println(WiFi.softAPIP()); // Prints 192.168.4.1
  } else {
    Serial.println("Access Point Failed!");
  }
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  //server.begin();
  display->showWifi(ssid, password, IP);
  startWebServer();
}

void setup() {


  Serial.begin(921600);

  pinMode(ELS_PAD_H2, INPUT_PULLDOWN);
  pinMode(ELS_PAD_V2, OUTPUT);
  digitalWrite(ELS_PAD_V2, 1);

  WebSettings* webSettings = getWebSettings();
  LatheConfig* config = getLatheSettings();

  bool checks = (webSettings->check == CHECKVALUE) && (config->check == CHECKVALUE);

  // Capture and clear the RTC-memory setup request immediately, before we
  // might branch into runWifiSettings() (which never returns to the normal
  // boot path) — otherwise a menu-triggered reboot into setup would loop
  // back into setup mode forever on every subsequent boot.
  bool wantSetup = (bootToSetup == BOOT_TO_SETUP_MAGIC);
  bootToSetup = 0;

  if (digitalRead(ELS_PAD_H2) == 1 || !checks || wantSetup) {
    display = new Display();
    Serial.println("AP setting mode\n");
    runWifiSettings();
  } else {


    Serial.printf("SSID %s\n", webSettings->ssid);
    Serial.printf("password %s\n", webSettings->password);
    Serial.printf("url %s\n", webSettings->url);


    LatheConfigDerived* derivedConfig = new LatheConfigDerived(config);

    spindle = new Spindle(ELS_SPINDLE_ENCODER_A, ELS_SPINDLE_ENCODER_B, derivedConfig);


    leadscrew = new Leadscrew(derivedConfig, spindle,
      &leadscrewIOImpl,
      derivedConfig->accellerationPulseSec(),
      derivedConfig->leadscrewInitialPulseDelay(),
      derivedConfig->stepperPpr() * derivedConfig->gearboxRatio(),
      derivedConfig->leadscrewPitchMm(), derivedConfig->spindleEncoderPpr());

    keyArray = new KeyArray(leadscrew);
    keyPad = new ButtonPad(spindle, leadscrew, keyArray);
    // ButtonPad owns the UiState; the display only reads it, to know which
    // selector overlay to show. Both run on the DisplayTask (displayLoop()
    // below calls keyPad->handle() then display->update()), so this is a
    // same-task read and needs no volatile or GlobalState round trip -- but it
    // does mean ButtonPad MUST be constructed first, as it is here: the
    // reference is taken now and held for the life of the display.
    display = new Display(spindle, leadscrew, &keyPad->ui());



    // config - compile time checks for safety
    CHECK_BOUNDS(DEFAULT_METRIC_THREAD_PITCH_IDX, threadPitchMetric,
      "DEFAULT_METRIC_THREAD_PITCH_IDX out of bounds");
    CHECK_BOUNDS(DEFAULT_METRIC_FEED_PITCH_IDX, feedPitchMetric,
      "DEFAULT_METRIC_FEED_PITCH_IDX out of bounds");
    CHECK_BOUNDS(DEFAULT_IMPERIAL_THREAD_PITCH_IDX, threadPitchImperial,
      "DEFAULT_IMPERIAL_THREAD_PITCH_IDX out of bounds");
    CHECK_BOUNDS(DEFAULT_IMPERIAL_FEED_PITCH_IDX, feedPitchImperial,
      "DEFAULT_IMPERIAL_FEED_PITCH_IDX out of bounds");

    // Pinmodes


#ifdef ELS_USE_RMT
    rmt_obj_t* leadscreRMT = rmtInit(ELS_LEADSCREW_STEP, true, RMT_MEM_64);
    leadscrew->setRMT(leadscreRMT);
    rmtSetTick(leadscreRMT, 2500);
#else
    pinMode(ELS_LEADSCREW_STEP, OUTPUT); // step output pin
#endif

    pinMode(ELS_LEADSCREW_DIR, OUTPUT);  // direction output pin

#ifdef ELS_UI_ENCODER

#ifdef ELS_IND_GREEN
    pinMode(ELS_IND_GREEN, OUTPUT);
    pinMode(ELS_IND_RED, OUTPUT);
    pinMode(ELS_IND_BLUE, OUTPUT);
    digitalWrite(ELS_IND_BLUE, 0);
#endif
#endif

    pinMode(ELS_STEPPER_ENA, OUTPUT);
    digitalWrite(ELS_STEPPER_ENA, 0);

    // The discrete-button #else branch that used to sit here has been deleted.
    // It configured nine GPIOs from ELS_*_BUTTON names that no longer exist,
    // and ELS_USE_BUTTON_ARRAY is defined unconditionally in board.h, so it had
    // not been compiled in a long time. Worse than merely dead: after the Mk2
    // remap the ELS_*_BUTTON names are MATRIX CODES, not GPIO numbers, so
    // ELS_ENABLE_BUTTON would still have resolved here and quietly configured
    // the wrong pin. There is no discrete-button hardware left to support.

    // Display Initalisation

    display->init();

    leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());

    display->update();


    TaskHandle_t spindleTask;
    TaskHandle_t displayTask;
    //TaskHandle_t commsTask;
    // CORE ASSIGNMENT - the spindle loop goes on core 1, NOT core 0.
    //
    // Core 0 is PRO_CPU: it carries the WiFi/lwIP stack, the IPC task and most
    // of the ESP-IDF housekeeping. Arduino runs loop() on core 1 for exactly
    // that reason. SpindleTask is a hard-realtime busy-loop with no delay - it
    // must consume every spindle encoder count promptly, because a missed
    // iteration lets counts pile up and the next consumePosition() returns a
    // large delta. m_expectedPosition then leaps forward by that delta times
    // the ratio and the leadscrew rushes to catch up, overshoots, and settles
    // back. On a thread that is a visible jump followed by a correction.
    //
    // This was the other way round and produced exactly that symptom on the
    // lathe: audible jitter plus a large forward jump every few seconds.
    //
    // DisplayTask goes on core 0. It sleeps 100 ms per pass, so it costs core 0
    // little, and it takes the keypad GPIO interrupts with it (initPad() runs
    // inside that task specifically so the ISRs land on its core) - which keeps
    // interrupt load off the realtime core as well.
    //
    // Core 1 also hosts the Arduino loopTask, but in normal operation that just
    // vTaskDelay(1000)s forever and is priority 1 against SpindleTask's 24.
    xTaskCreatePinnedToCore(SpindleTask, "Spindle", 4096, NULL, 24 | portPRIVILEGE_BIT, &spindleTask, 1);
    xTaskCreatePinnedToCore(DisplayTask, "Display", 8000, NULL, 1, &displayTask, 0);
    //xTaskCreatePinnedToCore(comms_loop, "Comms", 16000, NULL, 10, &commsTask, 1);
    disableLoopWDT();
    esp_task_wdt_delete(xTaskGetHandle("IDLE0"));
    esp_task_wdt_delete(xTaskGetHandle("IDLE1"));
    esp_task_wdt_delete(spindleTask);
    esp_task_wdt_delete(displayTask);
    //esp_task_wdt_delete(commsTask);

    delay(2000);
  }
}

void loop() {
  if (configMode) {
    wifiLoop();
    // Once a device joins the setup AP, switch the display to the "connected"
    // screen that points the browser at the config page.
    if (!apClientConnected && WiFi.softAPgetStationNum() > 0) {
      apClientConnected = true;
      display->showConnected(WiFi.softAPIP());
    }
  } else {
    vTaskDelay(1000);
  }
}