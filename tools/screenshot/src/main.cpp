// tools/screenshot -- render one screen of the REAL Display class to a PNG.
//
//   elsshot <scene> [outdir]      render one scene
//   elsshot --list                print every scene name, one per line
//
// ONE SCENE PER PROCESS, deliberately. lv_init() is global and Display gates it
// behind a per-object `initialised` flag, so a second Display in the same
// process (which the Wi-Fi screens need) would re-initialise LVGL underneath a
// live object tree. Re-running the binary is free and keeps every scene's LVGL
// heap, timer list and style cache pristine, which is the whole point: a
// screenshot that only looks right because of state left over from the previous
// one proves nothing.
#include <cstdio>
#include <cstring>
#include <string>

#include "framebuffer.h"
#include "scenes.h"

// The clear value. Chosen so that any pixel LVGL never painted SCREAMS rather
// than blending in: 0xF81F is full red + full blue, which no palette entry in
// either theme comes near. main() counts what is left of it after the render,
// so "the image is blank" and "the image is fine" cannot be confused.
static const uint16_t kSentinel = 0xF81F;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: elsshot <scene>|--list [outdir]\n");
    return 2;
  }

  if (std::strcmp(argv[1], "--list") == 0) {
    const char* const* names = shot::sceneNames();
    for (size_t i = 0; i < shot::sceneCount(); i++) {
      std::printf("%s\n", names[i]);
    }
    return 0;
  }

  const std::string scene = argv[1];
  const std::string outDir = (argc > 2) ? argv[2] : ".";

  shot::clearFramebuffer(kSentinel);
  shot::resetFramebufferTouched();

  if (!shot::renderScene(scene.c_str())) {
    std::fprintf(stderr, "elsshot: unknown scene '%s'\n", scene.c_str());
    return 2;
  }

  if (!shot::framebufferTouched()) {
    std::fprintf(stderr, "elsshot: %s -- LVGL never flushed a single pixel\n",
                 scene.c_str());
    return 1;
  }

  const std::string path = outDir + "/" + scene + ".png";
  // panelSwap: the display code authors every colour pre-swapped for this
  // panel's R<->B wiring, so the PNG has to undo that to show what the operator
  // sees. See writePng()'s comment.
  if (!shot::writePng(path, true)) {
    std::fprintf(stderr, "elsshot: cannot write %s\n", path.c_str());
    return 1;
  }

  const shot::ImageStats st = shot::framebufferStats();
  const size_t unpainted = shot::countColour(kSentinel);
  // One machine-readable line per image, for the verification pass. A render
  // that silently failed reads as colours=1 / ink=0.0% / unpainted=76800.
  std::printf("%-26s colours=%-5zu ink=%5.1f%%  modal=0x%04X  unpainted=%zu\n",
              scene.c_str(), st.distinctColours, st.inkCoverage * 100.0,
              (unsigned)st.modalColour, unpainted);
  return unpainted == 0 ? 0 : 3;
}
