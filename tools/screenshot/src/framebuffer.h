// The renderer's stand-in for the ST7789 panel: a 320x240 RGB565 framebuffer
// that lv_tft_espi_create()'s host replacement flushes into.
//
// It behaves like the real panel in the one way that matters here: it is
// PERSISTENT. LVGL renders in LV_DISPLAY_RENDER_MODE_PARTIAL and only flushes
// the areas it invalidated, exactly as on the device, so the buffer accumulates
// the same way the panel's own memory does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace shot {

// Logical screen size. The device creates the LVGL display 240x320 (portrait,
// the panel's native orientation) and then calls
// lv_display_set_rotation(LV_DISPLAY_ROTATION_90); LVGL swaps the reported
// resolution and renders/flushes in the rotated space, leaving the physical
// rotation to the panel's own MADCTL. So the coordinates arriving at flush_cb
// are the 320x240 landscape ones -- which is what a photograph of the panel
// would show.
static const int FB_W = 320;
static const int FB_H = 240;

// Raw RGB565 store, indexed [y * FB_W + x].
uint16_t* framebuffer();

// Fill the whole buffer with one RGB565 value. Called before each scenario so a
// pixel LVGL never draws is visibly "not drawn" rather than left over from the
// previous scene.
void clearFramebuffer(uint16_t rgb565);

// True once at least one flush has landed.
bool framebufferTouched();
void resetFramebufferTouched();

// Write the framebuffer to `path` as a PNG.
//
// panelSwap: undo the panel's R<->B wiring. The display code authors EVERY
// colour pre-swapped to compensate for that wiring (see the DisplayPalette
// comment in lib/display/ST7789_320_240displaylvgl.cpp), so LVGL's framebuffer
// holds the pre-swap values and the panel is what turns them back. Swapping R
// and B on the way out is therefore what makes the PNG match the panel; without
// it every colour on the image is wrong in exactly the way the convention
// exists to cancel.
bool writePng(const std::string& path, bool panelSwap);

// Ink statistics for the verification pass: how many distinct colours the image
// contains and what fraction of pixels differ from the modal (background)
// colour. A silently-black render scores 1 colour / 0% coverage.
struct ImageStats {
  size_t distinctColours;
  double inkCoverage;   // 0..1
  uint16_t modalColour;
};
ImageStats framebufferStats();

// How many pixels still hold `rgb565`. Used against the clear sentinel to prove
// LVGL actually painted every pixel: a non-zero count is a hole in the render,
// not a design choice, because the panel has no transparent state.
size_t countColour(uint16_t rgb565);

}  // namespace shot
