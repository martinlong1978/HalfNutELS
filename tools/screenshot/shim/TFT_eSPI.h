// Host shim for <TFT_eSPI.h> (tools/screenshot).
//
// lib/display's header includes this, but the display code itself never names a
// TFT_eSPI object: the only thing it uses from the driver is
// lv_tft_espi_create(), which the renderer supplies itself
// (tools/screenshot/shim/lv_tft_espi_host.cpp) as a plain LVGL display with a
// capturing flush callback. So this header only has to exist and to carry the
// panel geometry macros that the device build passes as -D flags -- and even
// those are supplied on the command line, so they are only defaulted here.
//
// SHIM-ONLY. Nothing here is on the device build's include path.
#pragma once

#ifndef TFT_WIDTH
#define TFT_WIDTH 240
#endif
#ifndef TFT_HEIGHT
#define TFT_HEIGHT 320
#endif
