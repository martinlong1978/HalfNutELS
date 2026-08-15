// Host (native) stub for <Arduino.h>.
//
// TEST-ONLY: this header is only on the include path for the PlatformIO
// `native` env (see `build_flags = ... -I test/stubs`). It provides just enough
// of the Arduino / ESP32 surface for the business-logic libraries to compile
// and run on the host under googletest. It is NOT a faithful Arduino
// implementation.
//
// Key feature: a test-settable virtual clock behind micros()/millis() so that
// time-dependent code (Leadscrew pulse timing, Axis velocity) is deterministic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Virtual clock
// ---------------------------------------------------------------------------
// Backed by a function-local static so the header stays include-safe across
// many translation units without needing C++17 inline variables.
inline uint64_t &__els_mock_micros() {
  static uint64_t value = 0;
  return value;
}

// Test helpers (call these from test code to drive time forward).
inline void setMockMicros(uint64_t micros) { __els_mock_micros() = micros; }
inline void advanceMockMicros(uint64_t delta) { __els_mock_micros() += delta; }
inline void resetMockClock() { __els_mock_micros() = 0; }

inline unsigned long micros() {
  return static_cast<unsigned long>(__els_mock_micros());
}
inline unsigned long millis() {
  return static_cast<unsigned long>(__els_mock_micros() / 1000ULL);
}

inline void delay(unsigned long ms) { advanceMockMicros(static_cast<uint64_t>(ms) * 1000ULL); }
inline void delayMicroseconds(unsigned long us) { advanceMockMicros(us); }
inline void yield() {}

// ---------------------------------------------------------------------------
// min / max / constrain  (Arduino provides these as macros)
// ---------------------------------------------------------------------------
// IMPORTANT: <cmath> is included ABOVE, and on this toolchain it transitively
// pulls in <bits/stl_algobase.h> (which defines std::min/std::max). Because that
// header is parsed *before* these macros are defined, its include guard is
// already set, so later <algorithm> includes are skipped and the std:: versions
// are never mangled by the macros. Defining them as macros (rather than global
// templates) avoids overload ambiguity with std:: in translation units that do
// `using namespace std;` (e.g. leadscrew.cpp) while still satisfying units that
// call bare min()/max() without any std:: in scope (e.g. globalstate.cpp).
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef constrain
#define constrain(amt, low, high) \
  ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

// ---------------------------------------------------------------------------
// Bit helpers (board.h defines ELS_*_BIT as BITn; only used by hardware code
// that is not compiled on host, but provided defensively).
// ---------------------------------------------------------------------------
#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif
#ifndef BIT25
#define BIT25 BIT(25)
#endif
#ifndef BIT26
#define BIT26 BIT(26)
#endif

// ---------------------------------------------------------------------------
// Serial no-op (no compiled business-logic TU actually emits to it, but the
// symbol is provided for completeness).
// ---------------------------------------------------------------------------
struct __SerialStub {
  void begin(long) {}
  void begin(long, int) {}
  void end() {}
  void flush() {}
  template <class... A> void print(A...) {}
  template <class... A> void println(A...) {}
  void println() {}
  template <class... A> int printf(A...) { return 0; }
  template <class... A> size_t write(A...) { return 0; }
  operator bool() const { return true; }
};
static __SerialStub Serial;

// ---------------------------------------------------------------------------
// ESP32 RMT shim.
//
// board.h unconditionally defines ELS_USE_RMT, so leadscrew.{h,cpp} reference
// the RMT types/functions even on host. These stubs let it compile; rmtWrite()
// is a no-op that reports success, which mirrors the real device's "a pulse was
// emitted" path (Leadscrew::sendPulse() returns true under ELS_USE_RMT).
// ---------------------------------------------------------------------------
typedef struct {
  int duration0;
  int level0;
  int duration1;
  int level1;
} rmt_data_t;

typedef struct rmt_obj_s rmt_obj_t;

inline bool rmtWrite(rmt_obj_t *, rmt_data_t *, size_t) { return true; }
