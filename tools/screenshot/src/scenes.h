// The scenario table: one named machine state per PNG.
//
// A scene is nothing but "put the rig into this state, then let Display render
// it". No display internals are touched -- every scene drives the SAME public
// inputs the firmware drives (GlobalState setters, Leadscrew stop/position
// setters, Spindle pulses over the virtual clock, and UiState::handleKey for
// focus), so what comes out is what the panel would show, not a posed mock-up.
#pragma once

#include <cstddef>

namespace shot {

// Renders `name` into the framebuffer. Returns false for an unknown name.
// Builds and tears down its own rig, so exactly one call per process.
bool renderScene(const char* name);

// The full ordered scene list, for the driver script and for `--list`.
const char* const* sceneNames();
size_t sceneCount();

}  // namespace shot
