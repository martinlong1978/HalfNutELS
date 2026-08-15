// Host (native) stub for <ESP32Encoder.h>.
//
// TEST-ONLY (native env include path). spindle.h declares an `ESP32Encoder
// m_encoder;` member, so the type must be complete for Spindle to compile on
// host. The native Spindle double (TestSpindle.cpp) never touches the encoder,
// so an empty class is sufficient. The real hardware implementation
// (ESPSpindle.cpp) is excluded from the native build.
#pragma once

#include <cstdint>

enum class puType { up, down, none };

class ESP32Encoder {
 public:
  static puType useInternalWeakPullResistors;

  ESP32Encoder() {}
  void attachFullQuad(int, int) {}
  void attachHalfQuad(int, int) {}
  int64_t getCount() { return 0; }
  int64_t getAndClearCount() { return 0; }
  void clearCount() {}
  void setCount(int64_t) {}
};
