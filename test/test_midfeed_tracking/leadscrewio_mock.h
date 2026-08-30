// Test double for the LeadscrewIO hardware interface. Records the last written
// step/dir pin states so tests can observe what Leadscrew commanded.
#pragma once

#include "leadscrew_io.h"

class LeadscrewIOMock : public LeadscrewIO {
 public:
  uint8_t m_stepPinState = 0;
  uint8_t m_dirPinState = 0;

  void writeStepPin(uint8_t state) override { m_stepPinState = state; }
  void writeDirPin(uint8_t state) override { m_dirPinState = state; }
  uint8_t readStepPin() override { return m_stepPinState; }
  uint8_t readDirPin() override { return m_dirPinState; }
};
