#ifndef ELS_BOARD_H
#define ELS_BOARD_H

// Board and pin definitions are all in here

// OTA update source. The device pulls firmware from the "latest" GitHub release
// of this repo. Derived URLs (built in code, see ESPCommsManager.cpp):
//   API:      https://api.github.com/repos/<REPO>/releases/latest
//   Download: https://github.com/<REPO>/releases/latest/download/<ASSET>
// The download asset MUST be named OTA_ASSET_NAME so the permalink resolves.
#define OTA_GITHUB_REPO "martinlong1978/TeensyELS"
#define OTA_ASSET_NAME "elstft.bin"

#define ELS_SPINDLE_ENCODER_A 35
#define ELS_SPINDLE_ENCODER_B 34

#define ELS_UI_ENCODER

#ifdef ELS_UI_ENCODER
#define ELS_UI_ENCODER_A 39  
#define ELS_UI_ENCODER_B 36  
#define ELS_IND_RED 22   
#define ELS_IND_GREEN 21  
#define ELS_IND_BLUE 12   
#endif

#define ELS_USE_RMT
#define ELS_LEADSCREW_STEP 25 
#define ELS_LEADSCREW_STEP_BIT BIT25
#define ELS_LEADSCREW_DIR 26
#define ELS_LEADSCREW_DIR_BIT BIT26

#define ELS_STEPPER_ENA 17

#define ELS_RATE_INCREASE_BUTTON 17
#define ELS_RATE_DECREASE_BUTTON 9
#define ELS_MODE_CYCLE_BUTTON 33
#define ELS_THREAD_SYNC_BUTTON 10
#define ELS_HALF_NUT_BUTTON 18
#define ELS_ENABLE_BUTTON 34
#define ELS_LOCK_BUTTON 12
#define ELS_JOG_LEFT_BUTTON 20
#define ELS_JOG_RIGHT_BUTTON 36
#define ELS_USE_BUTTON_ARRAY


#if defined(ELS_USE_BUTTON_ARRAY)
#define ELS_PAD_H1 32
#define ELS_PAD_H2 33
#define ELS_PAD_H3 2

#define ELS_PAD_V1 13
#define ELS_PAD_V2 14
#define ELS_PAD_V3 15
#endif

/**
 * Display
 *
 * This setting allows you to select what type of display you want to use.
 * The selection will hopefully grow as time goes on!
 *
 * Options:
 *   SSD1306_128_64: 128x64 oled
 *   ST7789_240_135
 */

#define SSD1306_128_64 0
#define ST7789_240_135 1
#define ST7789_240_135_LVGL 2

#define ELS_DISPLAY ST7789_240_135_LVGL
//#define ELS_DISPLAY SSD1306_128_64

#if ELS_DISPLAY == SSD1306_128_64
 // define this if you have a dedicated pin for the oled reset
#define PIN_DISPLAY_RESET -1
#endif

#endif