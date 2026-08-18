// Host replacement for LVGL's TFT_eSPI display driver
// (lvgl/src/drivers/display/tft_espi/lv_tft_espi.cpp, which is excluded from
// this build because it opens a real SPI panel).
//
// Same signature, same LVGL configuration -- lv_display_create() at the given
// resolution, the caller's buffer, LV_DISPLAY_RENDER_MODE_PARTIAL -- so
// Display::initDisplay() is left exactly as it is on the device. The ONLY
// difference is the flush callback, which copies the rendered area into the
// renderer's framebuffer instead of pushing it down SPI.
//
// The device's driver additionally forwards LV_EVENT_RESOLUTION_CHANGED to
// TFT_eSPI::setRotation(). That is deliberately NOT reproduced: the rotation is
// physical on the device (the panel's MADCTL), so LVGL renders and flushes in
// the ROTATED coordinate space either way. Dropping the forward leaves the
// pixels arriving here in exactly the landscape 320x240 space a photograph of
// the panel would show.
//
// SHIM-ONLY. Nothing here is on the device build's include path.
#include <lvgl.h>

#include "../src/framebuffer.h"

namespace shot {
void framebufferMarkTouched();
}

static void hostFlushCb(lv_display_t* disp, const lv_area_t* area,
                        uint8_t* px_map) {
  const int32_t x1 = area->x1;
  const int32_t y1 = area->y1;
  const int32_t x2 = area->x2;
  const int32_t y2 = area->y2;
  const int32_t w = x2 - x1 + 1;

  const uint16_t* src = (const uint16_t*)px_map;
  uint16_t* fb = shot::framebuffer();

  for (int32_t y = y1; y <= y2; y++) {
    for (int32_t x = x1; x <= x2; x++) {
      if (x < 0 || y < 0 || x >= shot::FB_W || y >= shot::FB_H) {
        continue;
      }
      fb[y * shot::FB_W + x] = src[(y - y1) * w + (x - x1)];
    }
  }
  shot::framebufferMarkTouched();
  lv_display_flush_ready(disp);
}

extern "C" lv_display_t* lv_tft_espi_create(uint32_t hor_res, uint32_t ver_res,
                                            void* buf,
                                            uint32_t buf_size_bytes) {
  lv_display_t* disp = lv_display_create(hor_res, ver_res);
  if (disp == NULL) {
    return NULL;
  }
  lv_display_set_flush_cb(disp, hostFlushCb);
  lv_display_set_buffers(disp, buf, NULL, buf_size_bytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  return disp;
}
