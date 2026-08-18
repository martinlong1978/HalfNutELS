#include "keyscan.h"

// EVERY member is initialised. This object lives inside KeyArray, which is
// heap-allocated in main.cpp, and the heap is not zeroed - the hazard CLAUDE.md
// records and the one that produced the original dead-buttons bug (an
// uninitialised keycodeMillis made the old debounce permanently true).
KeyScanner::KeyScanner()
    : m_candidate(0),
      m_candidateSinceMs(0),
      m_stable(0),
      m_stableSinceMs(0),
      m_holdFired(false),
      m_bounceRejects(0) {}

namespace {

// Append if there is room. Every emit goes through this, so the caller's
// maxOut is honoured on every path rather than on the ones we remembered.
inline void push(KeyScanOut* out, int maxOut, int& n, int code, int event) {
  if (n < maxOut) {
    out[n].code = code;
    out[n].event = event;
    n++;
  }
}

}  // namespace

int KeyScanner::update(int rawCode, unsigned long nowMs, KeyScanOut* out,
                       int maxOut) {
  int n = 0;
  if (out == 0 || maxOut <= 0) {
    return 0;
  }

  // -------------------------------------------------------------------------
  // 1. INTEGRATE. A reading has to persist for kKeyDebounceMs before it is
  //    believed. This is the whole debounce, and note what it is NOT: it does
  //    not gate any hardware reconfiguration, and it is not shared between
  //    press and release. It delays recognition and nothing else, so it can
  //    never lose a transition - the next sample re-reads the whole matrix
  //    whatever happened here (docs/keypad-audit.md §1).
  // -------------------------------------------------------------------------
  if (rawCode != m_candidate) {
    // A new reading restarts the clock. If the PREVIOUS candidate never made it
    // to stable, that is a bounce, and it is counted: "how many readings were
    // rejected" is the only number that says whether kKeyDebounceMs suits this
    // hardware, and it cannot be inferred from outside.
    if (m_candidate != m_stable) {
      m_bounceRejects++;
    }
    m_candidate = rawCode;
    m_candidateSinceMs = nowMs;
  }

  // Unsigned difference throughout, so a millis() wrap mid-press is a non-event
  // rather than an instant threshold crossing.
  const bool settled = (nowMs - m_candidateSinceMs) >= kKeyDebounceMs;

  // -------------------------------------------------------------------------
  // 2. PROMOTE, and emit whatever the change of stable code implies.
  // -------------------------------------------------------------------------
  if (settled && m_candidate != m_stable) {
    // Retire the outgoing key first, always, so a roll from one key straight to
    // another can never leave UiState holding a Press with no matching Release.
    // That pairing is load-bearing: the arrow Press starts a dead-man jog and
    // the Release is what stops it.
    if (m_stable != 0) {
      // Click only if no Hold fired for this press - "no Click after a Hold" is
      // the rule UiState's whole gesture model rests on (see the header comment
      // of lib/ui/uistate.cpp). Driven by the flag rather than re-derived from
      // the clock, so a press held across a millis() wrap cannot produce both.
      if (!m_holdFired) {
        push(out, maxOut, n, m_stable, KS_CLICKED);
      }
      push(out, maxOut, n, m_stable, KS_RELEASED);
    }

    m_stable = m_candidate;
    m_stableSinceMs = nowMs;
    m_holdFired = false;

    if (m_stable != 0) {
      push(out, maxOut, n, m_stable, KS_PRESSED);
    }
    return n;
  }

  // -------------------------------------------------------------------------
  // 3. THE HOLD. Once per press, at the threshold, for as long as the key stays
  //    down. Deliberately after the promotion above so a key cannot be promoted
  //    and held in the same sample.
  // -------------------------------------------------------------------------
  if (m_stable != 0 && !m_holdFired &&
      (nowMs - m_stableSinceMs) >= kKeyHoldMs) {
    m_holdFired = true;
    push(out, maxOut, n, m_stable, KS_HELD);
  }

  return n;
}
