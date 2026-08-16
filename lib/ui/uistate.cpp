// DELIBERATELY UNIMPLEMENTED STUB.
//
// Feature set D is being written test-first: test/test_uistate/ pins the whole
// contract from docs/ux-redesign.md §1-§6, and this file exists only so those
// tests LINK. Every method returns a neutral value, so the suite fails on
// assertions rather than on link errors.
//
// The implementer replaces the bodies below. Do not change uistate.h - the
// tests are written against that exact API.
#include "uistate.h"

// Out-of-line definitions for the in-class-initialised static constants, so
// odr-use (binding to a const reference, e.g. inside EXPECT_EQ) links under
// pre-C++17 as well.
const unsigned long UiState::kFocusTimeoutMs;
const int UiState::kMenuItemCount;

UiState::UiState()
    : m_focus(UiFocus::Jog),
      m_menuOpen(false),
      m_menuIndex(0),
      m_lastActivityMs(0),
      m_runToStopDir(0),
      m_jogDir(0) {}

UiFocus UiState::focus() const { return UiFocus::Jog; }

bool UiState::menuOpen() const { return false; }

int UiState::menuIndex() const { return 0; }

UiIntent UiState::handleKey(UiKey key, UiKeyEvent ev, const UiContext& ctx,
                            unsigned long nowMs) {
  (void)key;
  (void)ev;
  (void)ctx;
  (void)nowMs;
  return UiIntent::None;
}

bool UiState::tick(unsigned long nowMs) {
  (void)nowMs;
  return false;
}
