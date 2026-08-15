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

  if (digitalRead(ELS_PAD_H2) == 1 || !checks) {
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
    display = new Display(spindle, leadscrew);



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

#ifdef ELS_USE_BUTTON_ARRAY
#else
    pinMode(ELS_RATE_INCREASE_BUTTON, INPUT_PULLUP);  // rate Inc
    pinMode(ELS_RATE_DECREASE_BUTTON, INPUT_PULLUP);  // rate Dec
    pinMode(ELS_MODE_CYCLE_BUTTON, INPUT_PULLUP);     // mode cycle
    pinMode(ELS_THREAD_SYNC_BUTTON, INPUT_PULLUP);    // thread sync
    pinMode(ELS_HALF_NUT_BUTTON, INPUT_PULLUP);       // half nut
    pinMode(ELS_ENABLE_BUTTON, INPUT_PULLUP);         // enable toggle
    pinMode(ELS_LOCK_BUTTON, INPUT_PULLUP);           // lock toggle
    pinMode(ELS_JOG_LEFT_BUTTON, INPUT_PULLUP);       // jog left
    pinMode(ELS_JOG_RIGHT_BUTTON, INPUT_PULLUP);      // jog right
#endif

    // Display Initalisation

    display->init();

    leadscrew->setTargetPitchMM(globalState->getCurrentFeedPitch());

    display->update();


    TaskHandle_t spindleTask;
    TaskHandle_t displayTask;
    //TaskHandle_t commsTask;
    xTaskCreatePinnedToCore(SpindleTask, "Spindle", 4096, NULL, 24 | portPRIVILEGE_BIT, &spindleTask, 0);
    xTaskCreatePinnedToCore(DisplayTask, "Display", 8000, NULL, 1, &displayTask, 1);
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