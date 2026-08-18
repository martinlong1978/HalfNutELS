#include "framebuffer.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace shot {

static uint16_t g_fb[FB_W * FB_H];
static bool g_touched = false;

uint16_t* framebuffer() { return g_fb; }

void clearFramebuffer(uint16_t rgb565) {
  for (int i = 0; i < FB_W * FB_H; i++) {
    g_fb[i] = rgb565;
  }
}

bool framebufferTouched() { return g_touched; }
void resetFramebufferTouched() { g_touched = false; }

// Called by the host lv_tft_espi_create()'s flush callback.
void framebufferMarkTouched() { g_touched = true; }

// --- RGB565 -> RGB888 --------------------------------------------------------
// Bit replication (not a shift-and-zero-fill), so 0x1F maps to 255 and the
// image's white is actually white.
static inline void expand565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
  const uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
  const uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
  const uint8_t b5 = (uint8_t)(c & 0x1F);
  r = (uint8_t)((r5 << 3) | (r5 >> 2));
  g = (uint8_t)((g6 << 2) | (g6 >> 4));
  b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// --- A dependency-free PNG writer -------------------------------------------
// zlib's "stored" (uncompressed) deflate blocks are legal DEFLATE, so a valid
// PNG needs no compressor at all: a 2-byte zlib header, a run of stored blocks,
// and an Adler-32. The files are ~230 KB each, which is irrelevant for a
// screenshot tool and buys a build with no third-party dependency.

static uint32_t crcTable[256];
static bool crcTableReady = false;

static void makeCrcTable() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    crcTable[n] = c;
  }
  crcTableReady = true;
}

static uint32_t crc32Of(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
  if (!crcTableReady) makeCrcTable();
  for (size_t i = 0; i < len; i++) {
    crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

static void push32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((uint8_t)(x >> 24));
  v.push_back((uint8_t)(x >> 16));
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}

static void pushChunk(std::vector<uint8_t>& out, const char type[4],
                      const std::vector<uint8_t>& payload) {
  push32(out, (uint32_t)payload.size());
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  const uint32_t crc = crc32Of(&out[start], out.size() - start) ^ 0xFFFFFFFFu;
  push32(out, crc);
}

bool writePng(const std::string& path, bool panelSwap) {
  // --- Colour type ------------------------------------------------------------
  // INDEXED (colour type 3) whenever the image fits in 256 colours, which every
  // screen this tool renders does by a wide margin -- a UI drawn from a nine-
  // entry palette tops out around 80 once antialiasing is counted. That is one
  // index byte per pixel instead of three, and with stored deflate blocks (no
  // compressor, see below) the file size IS the pixel data: 77 KB per image
  // rather than 226 KB. Across the scene set that is the difference between
  // 2.4 MB and 7.2 MB added to git EVERY time the screens are re-rendered.
  //
  // Truecolour is kept as the fallback so a future screen with a gradient or a
  // photographic asset still writes a correct file rather than failing.
  std::map<uint16_t, size_t> counts;
  for (int i = 0; i < FB_W * FB_H; i++) {
    counts[g_fb[i]]++;
  }
  const bool indexed = counts.size() <= 256;

  // Palette, in ascending RGB565 order (map iteration), and the reverse lookup.
  std::vector<uint8_t> plte;
  std::map<uint16_t, uint8_t> indexOf;
  if (indexed) {
    uint8_t next = 0;
    for (std::map<uint16_t, size_t>::const_iterator it = counts.begin();
         it != counts.end(); ++it) {
      uint8_t r, g, b;
      expand565(it->first, r, g, b);
      plte.push_back(panelSwap ? b : r);
      plte.push_back(g);
      plte.push_back(panelSwap ? r : b);
      indexOf[it->first] = next++;
    }
  }

  // Raw scanlines: one filter byte (0 = None) then either one index per pixel
  // or an RGB triplet per pixel.
  std::vector<uint8_t> raw;
  raw.reserve((size_t)FB_H * (1 + (indexed ? 1 : 3) * FB_W));
  for (int y = 0; y < FB_H; y++) {
    raw.push_back(0);
    for (int x = 0; x < FB_W; x++) {
      const uint16_t c = g_fb[y * FB_W + x];
      if (indexed) {
        raw.push_back(indexOf[c]);
        continue;
      }
      uint8_t r, g, b;
      expand565(c, r, g, b);
      if (panelSwap) {
        raw.push_back(b);
        raw.push_back(g);
        raw.push_back(r);
      } else {
        raw.push_back(r);
        raw.push_back(g);
        raw.push_back(b);
      }
    }
  }

  // zlib stream around stored deflate blocks.
  std::vector<uint8_t> z;
  z.push_back(0x78);  // CM=8, CINFO=7
  z.push_back(0x01);  // FCHECK so (0x78<<8|0x01) % 31 == 0, no preset dict
  size_t offset = 0;
  while (offset < raw.size()) {
    const size_t n = (raw.size() - offset > 65535) ? 65535 : (raw.size() - offset);
    const bool last = (offset + n) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back((uint8_t)(n & 0xFF));
    z.push_back((uint8_t)(n >> 8));
    z.push_back((uint8_t)(~n & 0xFF));
    z.push_back((uint8_t)((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + n);
    offset += n;
  }
  uint32_t s1 = 1, s2 = 0;
  for (size_t i = 0; i < raw.size(); i++) {
    s1 = (s1 + raw[i]) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  push32(z, (s2 << 16) | s1);

  std::vector<uint8_t> png;
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.insert(png.end(), sig, sig + 8);

  std::vector<uint8_t> ihdr;
  push32(ihdr, (uint32_t)FB_W);
  push32(ihdr, (uint32_t)FB_H);
  ihdr.push_back(8);                  // bit depth
  ihdr.push_back(indexed ? 3 : 2);    // colour type: indexed, or truecolour RGB
  ihdr.push_back(0);  // compression: deflate
  ihdr.push_back(0);  // filter method
  ihdr.push_back(0);  // no interlace
  pushChunk(png, "IHDR", ihdr);
  if (indexed) {
    // PLTE must sit between IHDR and IDAT.
    pushChunk(png, "PLTE", plte);
  }
  pushChunk(png, "IDAT", z);
  pushChunk(png, "IEND", std::vector<uint8_t>());

  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  const size_t written = fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  return written == png.size();
}

ImageStats framebufferStats() {
  std::map<uint16_t, size_t> counts;
  for (int i = 0; i < FB_W * FB_H; i++) {
    counts[g_fb[i]]++;
  }
  ImageStats st;
  st.distinctColours = counts.size();
  st.modalColour = 0;
  size_t best = 0;
  for (std::map<uint16_t, size_t>::const_iterator it = counts.begin();
       it != counts.end(); ++it) {
    if (it->second > best) {
      best = it->second;
      st.modalColour = it->first;
    }
  }
  st.inkCoverage = 1.0 - ((double)best / (double)(FB_W * FB_H));
  return st;
}

size_t countColour(uint16_t rgb565) {
  size_t n = 0;
  for (int i = 0; i < FB_W * FB_H; i++) {
    if (g_fb[i] == rgb565) {
      n++;
    }
  }
  return n;
}

}  // namespace shot
