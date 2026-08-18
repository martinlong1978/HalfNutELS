#ifndef ELS_KEYSCAN_H
#define ELS_KEYSCAN_H

// Debounce and gesture recognition for the 3x3 keypad matrix.
//
// Pure C++: no Arduino, no ESP, no interrupts, no clock of its own (time
// arrives as `nowMs`). It takes the raw matrix code and returns key events, so
// the whole of the decision-making is host-testable the way lib/ui/uistate.cpp
// and lib/dro/dro.cpp are. src/keyarray.cpp keeps only the hardware: drive the
// columns, read the rows, hand the number over.
//
// WHY THIS EXISTS (docs/keypad-audit.md has the full account). The previous
// scheme was edge-triggered: a RISING interrupt for a press, a FALLING one for
// a release, re-armed as a side effect of each matrix scan, and gated by a
// single 10 ms "time since the last edge I acted on" lockout shared between
// both. Three consequences, all observed or provable:
//
//   * A release inside that 10 ms was swallowed - and because the code that
//     re-arms the interrupt sits AFTER the early return, the pad stayed armed
//     for FALLING on a line that was already low. No further edge could occur
//     and the KEYPAD WENT DEAD until the 1 s hold timer happened to rescan.
//     That is the reported unreliability.
//   * The scan called pinMode() and attachInterrupt() from inside the ISR,
//     neither of which is legal there, from flash, with no IRAM_ATTR - so a
//     keypress during any flash write (settings save, OTA, capture upload) ran
//     unmapped code.
//   * Tuning the 10 ms could not help: raising it swallows more releases,
//     lowering it lets bounce through as real events.
//
// So the debounce is by INTEGRATION - a code must read the same for several
// consecutive samples before it is believed - and the caller polls. Nothing is
// armed, so nothing can be left stranded; every sample sees the whole matrix,
// so both edges are always observable; and a bounce burst simply never reaches
// the threshold instead of consuming a timestamp something else depended on.
//
// THE EVENT VOCABULARY IS UNCHANGED, deliberately. UiState is built on exactly
// this sequence (see the header comment of lib/ui/uistate.cpp):
//
//     short press:  Press -> Click -> Release
//     long  press:  Press -> Hold  -> Release      (no Click after a Hold)
//
// so ButtonPad and UiState need no changes and their tests keep their meaning.

// Mirrors ButtonState in src/keyarray.h. Declared independently so this library
// stays free of the Arduino headers that file pulls in; the two are pinned
// together by a static_assert in src/keyarray.cpp.
enum KeyScanEvent {
  KS_NONE = 0,
  KS_PRESSED = 1,
  KS_CLICKED = 2,
  KS_HELD = 3,
  KS_RELEASED = 4,
};

struct KeyScanOut {
  int code;   // the matrix code the event belongs to
  int event;  // a KeyScanEvent
};

// --- Timing ----------------------------------------------------------------
//
// SCAN PERIOD is the caller's business (src/keyarray.cpp polls at 2 ms), but
// the thresholds below are expressed in milliseconds rather than sample counts
// so they stay correct if that rate ever changes, and so the tests can state
// them in the units the operator experiences.

// How long a reading must be stable before it is believed. Comfortably longer
// than the 1-3 ms of contact bounce these switches produce, and far below the
// ~50 ms floor of a deliberate human tap, so it rejects chatter without ever
// rejecting an intended press.
//
// NOTE what this number does NOT do, unlike the 10 ms it replaces: it does not
// gate any hardware reconfiguration, and it is not shared between press and
// release. It delays recognition; it can never lose an edge, because the next
// sample re-reads the whole matrix regardless.
static const unsigned long kKeyDebounceMs = 8;

// How long a key must be down before it counts as a Hold. UNCHANGED at 1 s:
// UiState's clear-both-stops gesture and the OK-hold DRO zero are both
// specified against it, and both are pinned by host tests.
static const unsigned long kKeyHoldMs = 1000;

// The most events one update() can produce. The worst case is a roll from one
// key straight to another without passing through zero, which retires the old
// key (Click + Release) and opens the new one (Press) - three. Four gives a
// margin the caller's array can rely on.
static const int kKeyScanMaxEvents = 4;

class KeyScanner {
 public:
  KeyScanner();

  // Feed one raw matrix reading; 0 means "nothing pressed".
  //
  // Returns the number of events written to `out` (0..kKeyScanMaxEvents). The
  // caller supplies the array; nothing is allocated and nothing is retained.
  //
  // `nowMs` must be non-decreasing. millis() wrapping every ~49 days is handled
  // by taking unsigned differences everywhere rather than comparing absolute
  // deadlines - the same rule the rest of the codebase follows.
  int update(int rawCode, unsigned long nowMs, KeyScanOut* out, int maxOut);

  // The key currently recognised as down, or 0. Test/diagnostic observation
  // point; nothing in the event path reads it.
  int stableCode() const { return m_stable; }

  // Counts, for the serial diagnostic described in docs/keypad-audit.md §6.
  // "How many readings were rejected as bounce" is the number that says whether
  // the debounce threshold is right for this hardware, and it cannot be
  // inferred from the outside.
  unsigned long bounceRejects() const { return m_bounceRejects; }

 private:
  // The last raw reading, and when it first appeared. A reading is promoted to
  // m_stable only once it has persisted for kKeyDebounceMs.
  int m_candidate;
  unsigned long m_candidateSinceMs;

  // The debounced code currently believed to be down (0 = none).
  int m_stable;

  // When m_stable was adopted, for the hold threshold, and whether the Hold has
  // already fired for this press. The flag is what implements "no Click after a
  // Hold": the release path tests it rather than re-deriving from the clock, so
  // a press held across a millis() wrap cannot produce both.
  unsigned long m_stableSinceMs;
  bool m_holdFired;

  unsigned long m_bounceRejects;
};

#endif  // ELS_KEYSCAN_H
