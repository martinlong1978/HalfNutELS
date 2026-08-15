#include <config.h>

#if ELS_DISPLAY == ST7789_240_135_LVGL
#include <ST7789_320_240displaylvgl.h>
#include <globalstate.h>

#define COLOUR_BRIGHTGREEN lv_color_hex(0x00FF00)
#define COLOUR_GREEN lv_color_hex(0x008800)
#define COLOUR_YELLOW lv_color_hex(0x0084FF)
#define COLOUR_RED lv_color_hex(0x0000FF)
#define COLOUR_DISABLED lv_color_hex(0xCCCCCC)

static uint32_t my_tick(void) {
  return millis();
}

void Display::initvars() {

}

// Append src to a Wi-Fi QR payload, escaping the characters that are special in
// the "WIFI:" URI scheme (\ ; , : ") with a leading backslash.
static void appendWifiQrEscaped(String& out, const char* src) {
  for (const char* p = src; *p != '\0'; ++p) {
    char c = *p;
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
      out += '\\';
    }
    out += c;
  }
}

void Display::showWifi(const char* ssid, const char* password, IPAddress ip) {
  initDisplay();

  // Left column: credentials as text (fallback if the QR can't be scanned).
  lv_obj_t* ssidLabel = lv_label_create(lv_screen_active());
  lv_obj_t* passwordLabel = lv_label_create(lv_screen_active());
  lv_obj_t* ipLabel = lv_label_create(lv_screen_active());

  lv_obj_t* ssidText = lv_label_create(lv_screen_active());
  lv_obj_t* passwordText = lv_label_create(lv_screen_active());
  lv_obj_t* ipText = lv_label_create(lv_screen_active());

  lv_obj_set_style_text_font(ssidLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(passwordLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(ipLabel, &lv_font_montserrat_14, 0);

  lv_obj_set_style_text_font(ssidText, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_font(passwordText, &lv_font_montserrat_26, 0);
  lv_obj_set_style_text_font(ipText, &lv_font_montserrat_26, 0);

  lv_obj_set_pos(ssidLabel, 10, 12);
  lv_obj_set_pos(ssidText, 10, 28);
  lv_obj_set_pos(passwordLabel, 10, 82);
  lv_obj_set_pos(passwordText, 10, 98);
  lv_obj_set_pos(ipLabel, 10, 152);
  lv_obj_set_pos(ipText, 10, 168);

  lv_label_set_text(ssidLabel, "Wifi SSID");
  lv_label_set_text(passwordLabel, "Password");
  lv_label_set_text(ipLabel, "IP Address");

  lv_label_set_text(ssidText, ssid);
  lv_label_set_text(passwordText, password);
  lv_label_set_text(ipText, ip.toString().c_str());

  // Right side: a Wi-Fi join QR code. Scanning it on a phone connects straight
  // to the setup AP (the captive portal then opens the config page).
  lv_obj_t* scanLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(scanLabel, &lv_font_montserrat_14, 0);
  lv_label_set_text(scanLabel, "Scan to join");
  lv_obj_set_pos(scanLabel, 196, 24);

  String qrPayload = "WIFI:S:";
  appendWifiQrEscaped(qrPayload, ssid);
  qrPayload += ";T:WPA;P:";
  appendWifiQrEscaped(qrPayload, password);
  qrPayload += ";;";

  lv_obj_t* wifiQr = lv_qrcode_create(lv_screen_active());
  lv_qrcode_set_size(wifiQr, 124);
  lv_qrcode_set_dark_color(wifiQr, lv_color_black());
  lv_qrcode_set_light_color(wifiQr, lv_color_white());
  // White background + padding gives the quiet zone scanners need.
  lv_obj_set_style_bg_color(wifiQr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(wifiQr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(wifiQr, 0, 0);
  lv_obj_set_style_pad_all(wifiQr, 6, 0);
  lv_qrcode_update(wifiQr, qrPayload.c_str(), qrPayload.length());
  lv_obj_set_pos(wifiQr, 182, 46);

  lv_timer_handler();

}

// Shown once a device has joined the setup AP. Deliberately NO QR: on phones the
// OS routes the plain browser over cellular while the AP is "captive", so a
// browser QR would mislead users into a route that won't work. Phones should use
// the OS "Sign in to network" prompt; the IP is shown for computers on the AP.
void Display::showConnected(IPAddress ip) {
  // Replace the join screen (LVGL is already initialised by showWifi()).
  lv_obj_clean(lv_screen_active());

  lv_obj_t* title = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, 0);
  lv_label_set_text(title, "Connected!");
  lv_obj_set_pos(title, 10, 14);

  lv_obj_t* msg = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
  lv_label_set_text(msg,
    "A device joined the setup network.\n\n"
    "On your phone, tap the\n"
    "\"Sign in to network\" prompt\n"
    "to open the configuration page.");
  lv_obj_set_pos(msg, 10, 72);

  lv_obj_t* ipLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(ipLabel, &lv_font_montserrat_14, 0);
  lv_label_set_text(ipLabel, "On a computer, browse to:");
  lv_obj_set_pos(ipLabel, 10, 178);

  lv_obj_t* ipText = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(ipText, &lv_font_montserrat_26, 0);
  lv_label_set_text(ipText, ip.toString().c_str());
  lv_obj_set_pos(ipText, 10, 198);

  lv_timer_handler();

}


void Display::initDisplay() {
  if (!initialised) {
    draw_buf = (uint32_t*)malloc(DRAW_BUF_SIZE);
    initialised = true;
  }

  lv_init();
  lv_tick_set_cb(my_tick);
  disp = lv_tft_espi_create(TFT_WIDTH, TFT_HEIGHT, draw_buf, DRAW_BUF_SIZE);
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
}

void Display::initialiseOta() {
  initOta = true;
  lv_obj_clean(lv_screen_active());

  lv_style_t knobStyle;

  lv_style_init(&knobStyle);

  updateLabel = lv_label_create(lv_screen_active());
  lv_label_set_text(updateLabel, "Updating...");
  lv_obj_set_style_text_font(updateLabel, &lv_font_montserrat_26, 0);
  lv_obj_align(updateLabel, LV_ALIGN_CENTER, 0, 0);

  updateSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(updateSlider, 280, 10);
  lv_obj_set_pos(updateSlider, 20, 150);

  lv_obj_set_style_opa(updateSlider, LV_OPA_0, LV_PART_KNOB);

  lv_slider_set_range(updateSlider, 0, 100);
  lv_obj_set_style_pad_all(updateSlider, 0, 0);



}

void Display::init() {


  initDisplay();

  pitchLabel = lv_label_create(lv_screen_active());
  rpmLabel = lv_label_create(lv_screen_active());
  leftStopRectObj = lv_obj_create(lv_screen_active());
  rightStopRectObj = lv_obj_create(lv_screen_active());
  lockedRectObj = lv_obj_create(lv_screen_active());
  enableRectObj = lv_obj_create(lv_screen_active());
  syncRectObj = lv_obj_create(lv_screen_active());
  pitchSlider = lv_slider_create(lv_screen_active());

  lv_obj_set_size(pitchSlider, 280, 10);
  lv_obj_set_pos(pitchSlider, 20, 70);

  feedSymbolObj = lv_image_create(lv_screen_active());
  leftStopObj = lv_image_create(leftStopRectObj);
  rightStopObj = lv_image_create(rightStopRectObj);
  lockedObj = lv_image_create(lockedRectObj);
  enableObj = lv_image_create(enableRectObj);
  syncObj = lv_image_create(syncRectObj);

  lv_obj_set_style_text_font(pitchLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_font(rpmLabel, &lv_font_montserrat_26, 0);

  lv_label_set_text(pitchLabel, "0.25mm");
  lv_label_set_text(rpmLabel, "3000rpm");

  lv_obj_set_size(leftStopRectObj, 40, 40);
  lv_obj_set_size(rightStopRectObj, 40, 40);
  lv_obj_set_size(lockedRectObj, 40, 40);
  lv_obj_set_size(enableRectObj, 40, 40);
  lv_obj_set_size(syncRectObj, 40, 40);

  lv_obj_set_style_bg_color(leftStopRectObj, (COLOUR_GREEN), 0);
  lv_obj_set_style_bg_color(rightStopRectObj, (COLOUR_DISABLED), 0);
  lv_obj_set_style_bg_color(lockedRectObj, (COLOUR_RED), 0);
  lv_obj_set_style_bg_color(enableRectObj, (COLOUR_DISABLED), 0);
  lv_obj_set_style_bg_color(syncRectObj, (COLOUR_DISABLED), 0);


  lv_obj_set_pos(rpmLabel, 160, 17);
  lv_obj_align(pitchLabel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_pos(leftStopRectObj, 10, 10);
  lv_obj_set_pos(rightStopRectObj, 60, 10);
  lv_obj_set_pos(lockedRectObj, 10, 190);
  lv_obj_set_pos(enableRectObj, 60, 190);
  lv_obj_set_pos(syncRectObj, 110, 190);
  lv_obj_set_pos(feedSymbolObj, 170, 166);


  lv_obj_set_style_pad_all(rightStopRectObj, 2, 0);
  lv_obj_set_style_pad_all(leftStopRectObj, 2, 0);
  lv_obj_set_style_pad_all(lockedRectObj, 2, 0);
  lv_obj_set_style_pad_all(enableRectObj, 2, 0);
  lv_obj_set_style_pad_all(syncRectObj, 2, 0);


  lv_image_set_src(feedSymbolObj, &threadSymbol);
  lv_image_set_src(leftStopObj, &leftstop);
  lv_image_set_src(rightStopObj, &rightstop);
  lv_image_set_src(lockedObj, &locked);
  lv_image_set_src(enableObj, &pauseSymbol);
  lv_image_set_src(syncObj, &syncSymbol);

  // "L"/"R" thread-hand indicator, sat over the thread mode icon.
  threadHandLabel = lv_label_create(lv_screen_active());
  lv_obj_set_style_text_font(threadHandLabel, &lv_font_montserrat_26, 0);
  lv_obj_set_pos(threadHandLabel, 282, 140);  // above-right of the bolt glyph
  lv_label_set_text(threadHandLabel, "");


}

void showWifi(const char* ssid, const char* password, IPAddress ip) {

}


void Display::update() {
  //  tft.fillScreen(TFT_BLACK); // Rely on localised blanking to avoid blink, for now.
  if (GlobalState::getInstance()->getDisplayReset()) {
    init();
  }
  lv_timer_handler();

  int bytes = GlobalState::getInstance()->getOTABytes();
  int length = GlobalState::getInstance()->getOTALength();
  if (GlobalState::getInstance()->hasOTA()) {
    if (!initOta) {
      initialiseOta();
    }
    drawOTA();

  } else {
    drawMode();
    drawPitch();
    drawLocked();
    drawEnabled();
    drawSpindleRpm();
    drawSyncStatus();
    drawStopStatus();
  }
  writeLed();
}

void Display::drawOTA() {
  int bytes = GlobalState::getInstance()->getOTABytes();
  int length = GlobalState::getInstance()->getOTALength();
  int percent = (((float)(bytes * 100)) / ((float)length));
  lv_slider_set_value(updateSlider, bytes > 0 ? percent : 0, LV_ANIM_OFF);
  if (percent > 99 && bytes > 0) {
    lv_label_set_text(updateLabel, "Rebooting...");
  } else {
    lv_label_set_text(updateLabel, "Updating...");
  }
}

void Display::drawSpindleRpm() {
  int rrpm = m_spindle->getEstimatedVelocityInRPM();
  int rpm = abs(rrpm);
  char rpmString[10];
  sprintf(rpmString, "%4dRPM", rpm);
  lv_label_set_text(rpmLabel, rpmString);
  lv_obj_set_style_text_color(rpmLabel, lv_color_hex(rpm < 0 ? 0xFF0000 : 0x000000), 0);
}

void Display::drawStopStatus() {
  if (m_leadscrew->getStopPositionState(LeadscrewStopPosition::LEFT) ==
    LeadscrewStopState::SET) {
    lv_obj_set_style_bg_color(leftStopRectObj, COLOUR_GREEN, 0);
  } else {
    lv_obj_set_style_bg_color(leftStopRectObj, COLOUR_DISABLED, 0);
  }
  if (m_leadscrew->getStopPositionState(LeadscrewStopPosition::RIGHT) ==
    LeadscrewStopState::SET) {
    lv_obj_set_style_bg_color(rightStopRectObj, COLOUR_GREEN, 0);
  } else {
    lv_obj_set_style_bg_color(rightStopRectObj, COLOUR_DISABLED, 0);
  }
}

void Display::drawSyncStatus() {
  GlobalThreadSyncState sync = GlobalState::getInstance()->getThreadSyncState();
  if (sync == GlobalThreadSyncState::SS_UNSYNC) {
    lv_obj_set_style_bg_color(syncRectObj, COLOUR_DISABLED, 0);
  } else {
    lv_obj_set_style_bg_color(syncRectObj, COLOUR_GREEN, 0);
  }
}

void Display::drawMode() {
  GlobalFeedMode mode = GlobalState::getInstance()->getFeedMode();
  if (mode == FM_JOG) {
    lv_image_set_src(feedSymbolObj, &jog);
    lv_label_set_text(threadHandLabel, "");
  } else if (mode == GlobalFeedMode::FM_FEED) {
    lv_image_set_src(feedSymbolObj, &feedSymbol);
    lv_label_set_text(threadHandLabel, "");
  } else if (mode == GlobalFeedMode::FM_THREAD) {
    // Right-hand thread: upright helix, marked "R".
    lv_image_set_src(feedSymbolObj, &threadSymbol);
    lv_label_set_text(threadHandLabel, "R");
  } else if (mode == GlobalFeedMode::FM_THREAD_REVERSE) {
    // Left-hand / reverse thread: vertically-flipped helix, marked "L".
    lv_image_set_src(feedSymbolObj, &threadSymbolReverse);
    lv_label_set_text(threadHandLabel, "L");
  }
}

void Display::drawPitch() {
  GlobalState* state = GlobalState::getInstance();
  GlobalUnitMode unit = state->getUnitMode();
  GlobalFeedMode mode = state->getFeedMode();

  int feedSelect = state->getFeedSelect();
  char pitch[10];
  if (mode == FM_JOG) {
    sprintf(pitch, "%d%s", (int)(state->getJogSpeed() * 100), "%");
    lv_slider_set_min_value(pitchSlider, 0);
    lv_slider_set_max_value(pitchSlider, sizeof(jogSpeeds) / sizeof(int));
    lv_slider_set_value(pitchSlider, state->getJogIndex() + 1, LV_ANIM_OFF);
  } else if (unit == GlobalUnitMode::METRIC) {
    if (mode == GlobalFeedMode::FM_THREAD || mode == GlobalFeedMode::FM_THREAD_REVERSE) {
      sprintf(pitch, "%.2fmm", threadPitchMetric[feedSelect]);

      lv_slider_set_min_value(pitchSlider, 0);
      lv_slider_set_max_value(pitchSlider, sizeof(threadPitchMetric) / sizeof(float));
      lv_slider_set_value(pitchSlider, feedSelect + 1, LV_ANIM_OFF);
    } else {
      sprintf(pitch, "%.2fmm", feedPitchMetric[feedSelect]);

      lv_slider_set_min_value(pitchSlider, 0);
      lv_slider_set_max_value(pitchSlider, sizeof(feedPitchMetric) / sizeof(float));
      lv_slider_set_value(pitchSlider, feedSelect + 1, LV_ANIM_OFF);
    }
  } else {
    if (mode == GlobalFeedMode::FM_THREAD || mode == GlobalFeedMode::FM_THREAD_REVERSE) {
      sprintf(pitch, "%dTPI", (int)threadPitchImperial[feedSelect]);

      lv_slider_set_min_value(pitchSlider, 0);
      lv_slider_set_max_value(pitchSlider, sizeof(threadPitchImperial) / sizeof(float));
      lv_slider_set_value(pitchSlider, feedSelect + 1, LV_ANIM_OFF);
    } else {
      sprintf(pitch, "%dth", (int)(feedPitchImperial[feedSelect] * 1000));

      lv_slider_set_min_value(pitchSlider, 0);
      lv_slider_set_max_value(pitchSlider, sizeof(feedPitchImperial) / sizeof(float));
      lv_slider_set_value(pitchSlider, feedSelect + 1, LV_ANIM_OFF);
    }
  }

  lv_label_set_text(pitchLabel, pitch);
}


void Display::drawJogSpeed() {
  // GlobalState* state = GlobalState::getInstance();
  // char pitch[10];
  // sprintf(pitch, "%d%s", (int)(state->getJogSpeed() * 100), "%");

  // if (!strcmp(pitch, m_jogString))return;
  // strcpy(m_jogString, pitch);
  // tft.fillRect(110, 22, 140, 40, TFT_BLACK);
  // tft.setCursor(110, 32);
  // tft.setTextSize(4);
  // tft.setTextColor(TFT_WHITE);
  // tft.print(pitch);
}

void Display::drawEnabled() {
  GlobalState* state = GlobalState::getInstance();
  GlobalMotionMode mode = state->getMotionMode();

  GlobalButtonLock lock = GlobalState::getInstance()->getButtonLock();
  switch (mode) {
  case GlobalMotionMode::MM_DISABLED:
    lv_obj_set_style_bg_color(enableRectObj, COLOUR_DISABLED, 0);
    lv_image_set_src(enableObj, &pauseSymbol);
    break;
  case GlobalMotionMode::MM_DECELLERATE:
    lv_obj_set_style_bg_color(enableRectObj, COLOUR_YELLOW, 0);
    lv_image_set_src(enableObj, &pauseSymbol);
    break;
  case GlobalMotionMode::MM_JOG_LEFT:
  case GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT:
    lv_obj_set_style_bg_color(enableRectObj, COLOUR_YELLOW, 0);
    lv_image_set_src(enableObj, &left);
    break;
  case GlobalMotionMode::MM_JOG_RIGHT:
  case GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT:
    lv_obj_set_style_bg_color(enableRectObj, COLOUR_YELLOW, 0);
    lv_image_set_src(enableObj, &right);
    break;
  case GlobalMotionMode::MM_ENABLED:
    lv_obj_set_style_bg_color(enableRectObj, COLOUR_GREEN, 0);
    lv_image_set_src(enableObj, &right);
    break;
  }
  updateLed();
}

void Display::writeLed() {
#ifdef ELS_UI_ENCODER
  int64_t time = micros() / 250000;
  EncoderColour c = time % 2 == 1 ? firstColour : secondColour;
  digitalWrite(ELS_IND_GREEN, (c & 2) == 2);
  digitalWrite(ELS_IND_RED, c & 1);
#endif
}


void Display::updateLed() {
#ifdef ELS_UI_ENCODER

  GlobalState* state = GlobalState::getInstance();
  GlobalMotionMode mode = state->getMotionMode();
  GlobalButtonLock lock = GlobalState::getInstance()->getButtonLock();

  switch (mode) {
  case GlobalMotionMode::MM_DISABLED:
    firstColour = lock == LK_LOCKED ? EC_RED : EC_NONE;
    secondColour = lock == LK_LOCKED ? EC_RED : EC_NONE;
    break;
  case GlobalMotionMode::MM_JOG_LEFT:
  case GlobalMotionMode::MM_JOG_RIGHT:
    firstColour = EC_YELLOW;
    secondColour = EC_YELLOW;
    break;
  case GlobalMotionMode::MM_ENABLED:
    firstColour = lock == LK_LOCKED ? EC_RED : EC_GREEN;
    secondColour = EC_GREEN;
    break;
  }
#endif

}

void Display::drawLocked() {
  GlobalButtonLock lock = GlobalState::getInstance()->getButtonLock();
  switch (lock) {
  case GlobalButtonLock::LK_LOCKED:
    lv_obj_set_style_bg_color(lockedRectObj, COLOUR_RED, 0);
    lv_image_set_src(lockedObj, &locked);
    break;
  case GlobalButtonLock::LK_UNLOCKED:
    lv_obj_set_style_bg_color(lockedRectObj, COLOUR_GREEN, 0);
    lv_image_set_src(lockedObj, &unlocked);
    break;
  }
  updateLed();
}
#endif