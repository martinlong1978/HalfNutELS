#include <config.h>

#if ELS_DISPLAY == ST7789_240_135_LVGL
#include <ST7789_320_240displaylvgl.h>
#include <globalstate.h>

// --- Runtime colour palette --------------------------------------------------
//
// docs/ux-redesign.md section 8 "Theme": dark and light are both legitimate
// looks on this panel depending on ambient light, so the palette is a runtime
// choice driven by LatheConfig::theme (THEME_DARK/THEME_LIGHT, latheconfig.h),
// not a compile-time one. Both instances below are compiled in (a few dozen
// bytes); Display::m_palette (set in the constructor, re-pointed only by
// setTheme()) selects the active one. This replaces the five COLOUR_* #defines
// that used to live here.
//
// *** COLOUR-ORDER TRAP -- READ BEFORE EDITING ANY VALUE BELOW ***
// This panel is wired R<->B swapped, so every lv_color_hex() value in BOTH
// palettes must be authored PRE-SWAPPED to compensate (CLAUDE.md "Display";
// this is exactly what the removed COLOUR_* constants used to do -- e.g. what
// reads as COLOUR_RED = lv_color_hex(0x0000FF) renders as red on this panel).
// NEVER paste a natural/designer RGB hex straight in here -- swap its R and B
// bytes first (0xRRGGBB -> 0xBBGGRR) and show the arithmetic in a comment, as
// done for the light palette's state colours below. A value with R==B (greys,
// black, white) is its own swap and needs no adjustment -- say so rather than
// leaving it looking unswapped-by-omission.
struct DisplayPalette {
  lv_color_t background;     // screen ground. Not painted by any code in this
                              // file yet (see Display::init()) -- the screen
                              // currently relies on LVGL's compiled-in default
                              // theme background, which this value mirrors for
                              // dark (see PALETTE_DARK) so a later feature set
                              // can wire it up without silently changing dark
                              // mode. Reserved for the theme/menu work.
  lv_color_t textPrimary;    // high-emphasis ink: the mode-symbol icon and the
                              // RPM label.
  lv_color_t textDim;        // low-emphasis ink. Not consumed by any draw call
                              // yet -- reserved, as background is, above.
  lv_color_t colourRun;      // armed / SET / synced state chips.
  lv_color_t colourCaution;  // jogging / decelerating state chips.
  lv_color_t colourFault;    // locked, and (after this pass) reverse-spindle
                              // RPM text.
  lv_color_t colourDisabled; // inactive state chips.
  lv_color_t iconInk;        // recolour for the 32x32 status icons, which sit
                              // on the coloured state chips above and need an
                              // ink that reads against all of them.
};

// Dark palette (THEME_DARK, the default): this reproduces *today's* rendering
// byte-for-byte. colourRun/colourCaution/colourFault/colourDisabled are the
// literal values the removed COLOUR_GREEN/YELLOW/RED/DISABLED #defines held;
// textPrimary/iconInk are the literal recolour hexes Display::init() used to
// hardcode inline. Nothing about dark-mode rendering changes by introducing
// this struct -- see each field's comment below for where it's still unused.
static const DisplayPalette PALETTE_DARK = {
  lv_color_hex(0xF5F5F5), // background: matches LVGL's implicit default theme
                          // screen colour today (LIGHT_COLOR_SCR = lv_palette_
                          // lighten(GREY, 4) in lv_theme_default.c, since
                          // lv_conf.h has LV_THEME_DEFAULT_DARK 0) -- R==B, no
                          // swap needed. Not painted anywhere in this pass.
  lv_color_hex(0x000000), // textPrimary: was the hardcoded black mode-icon/
                          // RPM ink -- R==B, no swap needed.
  lv_color_hex(0x616161), // textDim: unconsumed placeholder -- R==B, no swap.
  lv_color_hex(0x008800), // colourRun: was COLOUR_GREEN.
  lv_color_hex(0x0084FF), // colourCaution: was COLOUR_YELLOW.
  lv_color_hex(0x0000FF), // colourFault: was COLOUR_RED.
  lv_color_hex(0xCCCCCC), // colourDisabled: was COLOUR_DISABLED.
  lv_color_hex(0xFFFFFF), // iconInk: was the hardcoded white status-icon ink
                          // -- R==B, no swap needed.
};

// Light palette: not reachable yet -- no menu wires LatheConfig::theme to
// THEME_LIGHT, and setTheme() below is not called from anywhere. background/
// textPrimary/textDim are placeholders (unconsumed, same as in dark).
// colourRun/colourCaution/colourFault/colourDisabled come from
// docs/ux-redesign.md section 8 "Colour", which gives them pre-swapped
// already; the swap arithmetic (R<->B byte swap of the doc's natural hex) is
// reproduced here so it's checkable without cross-referencing the doc:
//   run:      natural #00C853 -> R=00,G=C8,B=53 -> swap R/B -> 53,C8,00 -> 0x53C800
//   caution:  natural #FFAB00 -> R=FF,G=AB,B=00 -> swap R/B -> 00,AB,FF -> 0x00ABFF
//   fault:    natural #FF3B30 -> R=FF,G=3B,B=30 -> swap R/B -> 30,3B,FF -> 0x303BFF
//   disabled: natural #6B7280 -> R=6B,G=72,B=80 -> swap R/B -> 80,72,6B -> 0x80726B
// These deliberately do NOT match PALETTE_DARK's state colours even though
// docs/ux-redesign.md section 8 "Theme" says "the accent/state hues are
// shared so only the ground and text tokens differ" -- that line describes an
// eventual full re-skin where dark's state colours would ALSO move to this
// vivid set, but this pass's hard constraint is that dark stays byte-for-byte
// identical to today, so dark keeps today's older/dimmer state hex values
// instead. Worth reconciling when the two palettes are next revisited.
static const DisplayPalette PALETTE_LIGHT = {
  lv_color_hex(0xFFFFFF), // background: white -- R==B, no swap needed.
  lv_color_hex(0x000000), // textPrimary: black -- R==B, no swap needed.
  lv_color_hex(0x757575), // textDim: mid grey -- R==B, no swap needed.
  lv_color_hex(0x53C800), // colourRun, see swap working above.
  lv_color_hex(0x00ABFF), // colourCaution, see swap working above.
  lv_color_hex(0x303BFF), // colourFault, see swap working above.
  lv_color_hex(0x80726B), // colourDisabled, see swap working above.
  lv_color_hex(0xFFFFFF), // iconInk: white, same reasoning as dark -- reads
                          // against all four state chip colours above.
};

static uint32_t my_tick(void) {
  return millis();
}

Display::Display(Spindle* spindle, Leadscrew* leadscrew) {
  this->m_spindle = spindle;
  this->m_leadscrew = leadscrew;
  this->m_globalState = GlobalState::getInstance();
  // Theme is picked once here, not re-read from config on every init() rebuild
  // (Display::update() calls init() whenever getDisplayReset() fires) -- if it
  // were re-read every time, a future runtime setTheme() call would just get
  // clobbered back to the config default on the very next rebuild it triggers.
  // Any value other than THEME_LIGHT falls back to dark, mirroring the same
  // safe-fallback pattern latheconfig.cpp uses for droDatum.
  uint8_t theme = (leadscrew != nullptr) ? leadscrew->getConfig()->theme() : THEME_DARK;
  this->m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
}

Display::Display() {
  // Used only for the Wi-Fi setup-mode screen (main.cpp runWifiSettings()),
  // via showWifi()/showConnected() -- init() (the themed dashboard built
  // below) is never called on this path, so m_spindle/m_leadscrew are
  // genuinely unused here. Per CLAUDE.md ("Constructors must initialise all
  // members" -- this object is heap-allocated with `new`, and the heap is not
  // zero-initialised) they must still be set explicitly rather than left
  // holding garbage, hence the explicit nullptr rather than omitting them.
  this->m_spindle = nullptr;
  this->m_leadscrew = nullptr;
  this->m_globalState = GlobalState::getInstance();
  this->m_palette = &PALETTE_DARK;  // no config available on this path -- default dark.
}

// Runtime theme switch. Not wired to any UI yet -- docs/ux-redesign.md
// section 8 "Theme" menu tile doesn't exist -- but reachable: re-points
// m_palette and asks Display::update() to rebuild the whole screen from
// scratch next tick via the existing getDisplayReset()/init() path
// (see Display::update() below), the same mechanism already used for the
// OTA <-> normal screen swap, so no new plumbing is needed.
void Display::setTheme(uint8_t theme) {
  m_palette = (theme == THEME_LIGHT) ? &PALETTE_LIGHT : &PALETTE_DARK;
  m_globalState->setDisplayReset();
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

  lv_obj_set_style_bg_color(leftStopRectObj, (m_palette->colourRun), 0);
  lv_obj_set_style_bg_color(rightStopRectObj, (m_palette->colourDisabled), 0);
  lv_obj_set_style_bg_color(lockedRectObj, (m_palette->colourFault), 0);
  lv_obj_set_style_bg_color(enableRectObj, (m_palette->colourDisabled), 0);
  lv_obj_set_style_bg_color(syncRectObj, (m_palette->colourDisabled), 0);


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

  // These icons were converted from I1 (2-entry palette, ink colour baked in) to A8
  // (alpha-only; colour comes from the image's recolor style). LVGL's A8 blend path
  // applies draw_dsc->recolor unconditionally (unlike other formats, which gate on
  // recolor_opa > LV_OPA_MIN), and the style default for LV_STYLE_IMAGE_RECOLOR is
  // black -- so without an explicit recolor here every one of these renders black.
  // Set it explicitly per object (not per source -- drawMode()/drawEnabled()/
  // drawLocked() swap lv_image_set_src at runtime on these same objects), sourced
  // from the active palette (m_palette) so the icons follow the theme: textPrimary
  // for the 128x64 mode symbol sitting on the general background, iconInk for the
  // 32x32 status icons sitting on the coloured state rectangles. In PALETTE_DARK
  // both equal the literal black/white this used to hardcode, so dark-mode
  // rendering is unchanged.
  lv_obj_set_style_image_recolor(feedSymbolObj, m_palette->textPrimary, 0);
  lv_obj_set_style_image_recolor_opa(feedSymbolObj, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(leftStopObj, m_palette->iconInk, 0);
  lv_obj_set_style_image_recolor_opa(leftStopObj, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(rightStopObj, m_palette->iconInk, 0);
  lv_obj_set_style_image_recolor_opa(rightStopObj, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(lockedObj, m_palette->iconInk, 0);
  lv_obj_set_style_image_recolor_opa(lockedObj, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(enableObj, m_palette->iconInk, 0);
  lv_obj_set_style_image_recolor_opa(enableObj, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(syncObj, m_palette->iconInk, 0);
  lv_obj_set_style_image_recolor_opa(syncObj, LV_OPA_COVER, 0);

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
  GlobalState* state = GlobalState::getInstance();
  switch (state->getOtaStatus()) {
  case OTA_CHECKING:
  case OTA_IDLE:
    // OTA_IDLE is the brief window before the OTA task sets CHECKING.
    lv_label_set_text(updateLabel, "Checking for updates...");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_NO_UPDATE:
    lv_label_set_text(updateLabel, "No update available");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_FAILED:
    lv_label_set_text(updateLabel, "Update failed");
    lv_slider_set_value(updateSlider, 0, LV_ANIM_OFF);
    break;
  case OTA_DOWNLOADING: {
    int bytes = state->getOTABytes();
    int length = state->getOTALength();
    int percent = length > 0 ? (int)(((float)(bytes * 100)) / ((float)length)) : 0;
    lv_slider_set_value(updateSlider, bytes > 0 ? percent : 0, LV_ANIM_OFF);
    if (percent > 99 && bytes > 0) {
      lv_label_set_text(updateLabel, "Rebooting...");
    } else {
      lv_label_set_text(updateLabel, "Updating...");
    }
    break;
  }
  }
}

void Display::drawSpindleRpm() {
  int rrpm = m_spindle->getEstimatedVelocityInRPM();
  int rpm = abs(rrpm);
  char rpmString[10];
  sprintf(rpmString, "%4dRPM", rpm);
  lv_label_set_text(rpmLabel, rpmString);
  // Bug fix (CLAUDE.md "Gotchas" / "Known open items" called this out as a
  // cosmetic colour-order miss, but it was worse than cosmetic): this used to
  // test `rpm < 0`, but `rpm` is abs(rrpm) two lines up, so that condition was
  // always false -- the "negative RPM" branch was dead code, and reverse
  // spindle always rendered the RPM text in the normal (black) colour. Fixed
  // to test the signed `rrpm`, and the colour is now `m_palette->colourFault`
  // (pre-swapped, renders true red) instead of the old un-swapped 0xFF0000
  // literal (which rendered blue on this R<->B-swapped panel). In PALETTE_DARK
  // the normal-case colour (m_palette->textPrimary) is still the literal
  // 0x000000 this hardcoded, so forward-spindle rendering is unchanged.
  lv_obj_set_style_text_color(rpmLabel, rrpm < 0 ? m_palette->colourFault : m_palette->textPrimary, 0);
}

void Display::drawStopStatus() {
  if (m_leadscrew->getStopPositionState(LeadscrewStopPosition::LEFT) ==
    LeadscrewStopState::SET) {
    lv_obj_set_style_bg_color(leftStopRectObj, m_palette->colourRun, 0);
  } else {
    lv_obj_set_style_bg_color(leftStopRectObj, m_palette->colourDisabled, 0);
  }
  if (m_leadscrew->getStopPositionState(LeadscrewStopPosition::RIGHT) ==
    LeadscrewStopState::SET) {
    lv_obj_set_style_bg_color(rightStopRectObj, m_palette->colourRun, 0);
  } else {
    lv_obj_set_style_bg_color(rightStopRectObj, m_palette->colourDisabled, 0);
  }
}

void Display::drawSyncStatus() {
  GlobalThreadSyncState sync = GlobalState::getInstance()->getThreadSyncState();
  if (sync == GlobalThreadSyncState::SS_UNSYNC) {
    lv_obj_set_style_bg_color(syncRectObj, m_palette->colourDisabled, 0);
  } else {
    lv_obj_set_style_bg_color(syncRectObj, m_palette->colourRun, 0);
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
    lv_obj_set_style_bg_color(enableRectObj, m_palette->colourDisabled, 0);
    lv_image_set_src(enableObj, &pauseSymbol);
    break;
  case GlobalMotionMode::MM_DECELLERATE:
    lv_obj_set_style_bg_color(enableRectObj, m_palette->colourCaution, 0);
    lv_image_set_src(enableObj, &pauseSymbol);
    break;
  case GlobalMotionMode::MM_JOG_LEFT:
  case GlobalMotionMode::MM_INTERACTIVE_JOG_LEFT:
    lv_obj_set_style_bg_color(enableRectObj, m_palette->colourCaution, 0);
    lv_image_set_src(enableObj, &left);
    break;
  case GlobalMotionMode::MM_JOG_RIGHT:
  case GlobalMotionMode::MM_INTERACTIVE_JOG_RIGHT:
    lv_obj_set_style_bg_color(enableRectObj, m_palette->colourCaution, 0);
    lv_image_set_src(enableObj, &right);
    break;
  case GlobalMotionMode::MM_ENABLED:
    lv_obj_set_style_bg_color(enableRectObj, m_palette->colourRun, 0);
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
    lv_obj_set_style_bg_color(lockedRectObj, m_palette->colourFault, 0);
    lv_image_set_src(lockedObj, &locked);
    break;
  case GlobalButtonLock::LK_UNLOCKED:
    lv_obj_set_style_bg_color(lockedRectObj, m_palette->colourRun, 0);
    lv_image_set_src(lockedObj, &unlocked);
    break;
  }
  updateLed();
}
#endif