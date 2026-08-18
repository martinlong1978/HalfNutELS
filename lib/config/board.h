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

// ---------------------------------------------------------------------------
// Keypad matrix codes (Mk2 layout, docs/ux-redesign.md Sec. 2)
//
// These are NOT GPIO numbers. The 3x3 matrix scan in KeyArray::getCodeFromArray()
// (src/keyarray.cpp:125,137-138) builds `code = a | b << 3`, where `a` is the H
// bitmask and `b` the V bitmask. Physical ROWS run along H and physical COLUMNS
// along V, so the codes read:
//
//            V1 (left)   V2 (centre)  V3 (right)
//   H1 (top)      9           17          33
//   H2 (mid)     10           18          34
//   H3 (bot)     12           20          36
//
// Mk2 assignment - selectors on top, the two actuators plus OK in the middle
// where the thumbs sit, machine state along the bottom:
//
//   MODE   RATE   STOPS
//    <-     OK     ->
//   HALT   MENU  ENABLE
//
// MODE / RATE / STOPS choose what the arrows drive (the focus, lib/ui/uistate.h);
// the arrows are the only actuators. ENABLE is machine state and is handled
// outside the focus model, in ButtonPad directly.
//
// Orientation ("H is rows, V is columns") is NOT provable from source - the scan
// only ever sees two bitmasks, and the boot gesture uses H2/V2, the centre key
// under either reading. It was confirmed against the physical panel by its
// owner: the top row reads 9, 17, 33 left to right. That is why the table above
// runs codes ACROSS each row rather than down each column.
//
// If the loom is ever rewired, the nine codes stay correct and distinct but the
// assignment transposes - the legends would read MODE / left-arrow / HALT down
// the left-hand column instead. Re-confirm by pressing each key and reading the
// code before making new caps.
//
// Code 18 must stay the centre key: src/main.cpp:144-160 samples
// ELS_PAD_H2 / ELS_PAD_V2 (i.e. code 18) at boot to enter Wi-Fi setup mode.
// Putting OK there keeps that gesture as "hold OK at power-on for setup".
#define ELS_MODE_BUTTON 9
#define ELS_RATE_BUTTON 17
#define ELS_STOPS_BUTTON 33
#define ELS_LEFT_BUTTON 10
#define ELS_OK_BUTTON 18
#define ELS_RIGHT_BUTTON 34
#define ELS_HALT_BUTTON 12
#define ELS_MENU_BUTTON 20
#define ELS_ENABLE_BUTTON 36
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