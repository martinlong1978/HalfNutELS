// Host shim for <Arduino.h>, for the screen renderer (tools/screenshot).
//
// The native unit-test stub (test/stubs/Arduino.h) already provides the virtual
// clock and the RMT/Serial no-ops that the business-logic libraries need, and it
// is the file the 314 host tests are written against, so it is REUSED verbatim
// rather than forked. This header only adds the extra Arduino surface that
// lib/display touches and the test stub has never needed: GPIO writes (the
// encoder LED in Display::writeLed()) and the String / IPAddress types used by
// Display::showWifi() / showConnected().
//
// SHIM-ONLY. Nothing here is on the device build's include path.
#pragma once

// The C++ library headers come FIRST, before the stub, and that order is
// load-bearing. The stub defines min/max as MACROS (it explains why at their
// definition), and libstdc++ declares member functions called `min`, `max` and
// `compare` -- so any <string>/<string_view>/<algorithm> parsed AFTER the macros
// fails to compile. The stub gets away with it only because it includes <cmath>
// itself, which happens to pull in the one header that matters for the unit
// tests. Anything this shim needs on top must therefore be pulled in above it.
#include <cstdio>
#include <string>

// Relative, not via -I: the screenshot build puts THIS directory ahead of
// test/stubs so that <Arduino.h> resolves here, which means an <Arduino.h>
// include from inside the stub would find itself.
#include "../../../test/stubs/Arduino.h"

// ---------------------------------------------------------------------------
// GPIO no-ops. Display::writeLed() drives the encoder's indicator LED on every
// update(); on host the pin writes simply go nowhere.
// ---------------------------------------------------------------------------
#ifndef INPUT
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#endif
#ifndef LOW
#define LOW 0
#define HIGH 1
#endif

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }

// ---------------------------------------------------------------------------
// String: only the members lib/display actually calls (operator+= on char and
// const char*, c_str(), length()). Backed by std::string.
// ---------------------------------------------------------------------------
class String {
 public:
  String() {}
  String(const char* s) : m_s(s ? s : "") {}
  String(const std::string& s) : m_s(s) {}

  String& operator+=(const char* s) { m_s += (s ? s : ""); return *this; }
  String& operator+=(char c) { m_s += c; return *this; }
  String& operator+=(const String& o) { m_s += o.m_s; return *this; }

  const char* c_str() const { return m_s.c_str(); }
  size_t length() const { return m_s.size(); }

 private:
  std::string m_s;
};

// ---------------------------------------------------------------------------
// IPAddress: constructed from four octets, rendered by toString(). That is the
// whole of what the Wi-Fi setup screen does with it.
// ---------------------------------------------------------------------------
class IPAddress {
 public:
  IPAddress() : a(0), b(0), c(0), d(0) {}
  IPAddress(uint8_t a_, uint8_t b_, uint8_t c_, uint8_t d_)
      : a(a_), b(b_), c(c_), d(d_) {}

  String toString() const {
    char buf[24];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)a, (unsigned)b,
             (unsigned)c, (unsigned)d);
    return String(buf);
  }

 private:
  uint8_t a, b, c, d;
};
