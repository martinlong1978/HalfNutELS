#ifndef ELS_ENCODERDETENTS_H
#define ELS_ENCODERDETENTS_H

#include <stdint.h>

// Raw quadrature counts -> whole detents, for the UI knob.
//
// Pure C++: no Arduino, no ESP, no hardware. It takes the free-running count
// out of the PCNT unit and returns the number of DETENTS the operator has
// turned since the last ask, so the whole of the arithmetic is host-testable
// the way lib/keyscan and lib/ui are. src/keyarray.cpp keeps only the
// hardware: attach the pins, read the counter, hand the number over.
//
// WHY THIS EXISTS - the knob used to skip items, sometimes several, and
// sometimes step BACKWARDS. It was decoded with ESP32Encoder::attachSingleEdge,
// which configures PCNT to count on ONE edge of A only:
//
//     pos_mode = PCNT_COUNT_DIS   // A rising:  ignored
//     neg_mode = PCNT_COUNT_INC   // A falling: count, direction from B's level
//
// Nothing there ever decrements, so CONTACT BOUNCE ACCUMULATES: a five-bounce
// burst on A is five counts, all the same sign, all real as far as the counter
// is concerned. That is the skipping. The direction came from a single sample
// of B's level at the instant A fell, with no confirmation, so a detent taken
// while B was mid-transition or bouncing counted the wrong way. That is the
// reversing. And a detent that happens to rest near A's threshold chatters
// while the knob is STILL, which is both symptoms at once.
//
// The PCNT glitch filter cannot reach any of it. Its value is in APB clock
// cycles and the library clamps it at 1023, so at 80 MHz the maximum possible
// filter is 12.79 us; mechanical bounce on this class of encoder runs 0.1-5 ms,
// one to three orders of magnitude longer. The filter was already at that
// ceiling. It stays there - it earns its place against EMI and against the
// ~80 ns GPIO36/39 SAR-ADC glitch erratum - but it was never the answer here.
//
// FULL QUADRATURE IS SELF-CORRECTING, which is the actual fix and the reason
// this class exists. With pos_mode = DEC and neg_mode = INC on both channels,
// A falling is +1 and A bouncing back up is -1: a bounce burst cancels itself
// IN HARDWARE and nets to the true position, whichever line bounced. The count
// is then 4 per detent instead of 1, and dividing that back down is all the
// work left - which is what is below.
//
// The 100 ms sample period in ButtonPad becomes an ADVANTAGE under this scheme
// rather than something to work around: the counter self-heals during the
// bounce, so a poll that lands afterwards only ever observes settled positions.
class EncoderDetents {
 public:
  // Raw counts per detent. Four for a full-quadrature decode of an encoder
  // that completes one quadrature cycle per detent, which is what E1 is; a
  // half-detent part would want two. Measure it rather than assume it - one
  // slow turn of twenty detents should move the raw count by exactly 20*this.
  explicit EncoderDetents(int countsPerDetent)
      : m_countsPerDetent(countsPerDetent > 0 ? countsPerDetent : 1),
        m_lastRaw(0),
        m_residue(0),
        m_pending(0),
        m_glitchDrops(0) {}

  // Adopt `rawCount` as the current position without reporting any movement.
  // Called once at construction, so the first update() measures from where the
  // counter actually was rather than from zero.
  void reset(int64_t rawCount) {
    m_lastRaw = rawCount;
    m_residue = 0;
    m_pending = 0;
  }

  // Whole detents since the last call. Positive is clockwise.
  int update(int64_t rawCount) {
    const int64_t delta = rawCount - m_lastRaw;
    m_lastRaw = rawCount;

    // DROP a wild delta, do not clamp it. ESP32Encoder runs the PCNT counter
    // to +/-INT16 and accumulates the wrap in a limit ISR, and a read racing
    // that ISR returns a value at the 16-bit boundary - the same artefact that
    // produced the "180 degree forward jump while threading" and that
    // Spindle::update() now drops (CLAUDE.md, motion gotchas). The old code
    // here CLAMPED it to +/-64 instead, which turns a counter artefact into 64
    // real menu steps. Half the 16-bit range cannot be reached by a thumb:
    // 16384 counts is 4096 detents in one 100 ms pass.
    if (delta > kGlitchLimit || delta < -kGlitchLimit) {
      m_glitchDrops++;
      return takePending();
    }

    // Sub-detent movement is HELD, not rounded away. A knob pushed two counts
    // off a detent and released snaps back by two: residue returns to zero and
    // nothing is ever reported, which is the behaviour the operator expects.
    // Truncation toward zero makes that symmetric in both directions.
    m_residue += (int)delta;
    const int whole = m_residue / m_countsPerDetent;
    m_residue -= whole * m_countsPerDetent;
    m_pending += whole;

    return takePending();
  }

  // Counter artefacts discarded. Judged from the machine rather than assumed,
  // like bounceRejects()/ringDrops(); it should stay at zero.
  unsigned long glitchDrops() const { return m_glitchDrops; }

  // Detents held back by the per-call bound, waiting for the next call.
  int pending() const { return m_pending; }

  // The most one call can return, AND the most that may still be owed once it
  // has returned (GitHub issue #5, Part 2, revised). ButtonPad replays one
  // UiState::handleKey() per detent in a `while` loop, so the return value
  // needs a bound - that half is unchanged. What changed is the residual:
  // it used to be kept in full and handed over next pass regardless of size,
  // so a directionally-biased noise burst could leave thousands of detents
  // to drain out kMaxPerCall at a time, for seconds, long after whatever
  // produced it was over - moving or not, because update() has no idea the
  // carriage exists and is called every pass regardless of motion state.
  //
  // The residual is now ALSO clamped to this bound: at most one further
  // pass of backlog may survive a call. This can cost a step - a burst
  // large enough to exceed 2*kMaxPerCall in one 100 ms pass loses the
  // excess rather than draining it out later - but that is the trade this
  // bound is FOR: at a 100 ms poll a human thumb cannot produce anywhere
  // near kMaxPerCall detents in one pass, let alone two, so anything beyond
  // that bound was never a real step to begin with (see kGlitchLimit's own
  // note: half the 16-bit range is 4096 detents in one pass). Discarding it
  // needs no context about WHY the detents arrived - alarm, UiFocus::Stops,
  // OTA, or genuine motion - which is exactly the question this class is
  // deliberately unable to ask. See test/test_encoderdetents for the
  // reasoning this replaced (grep "issue #5").
  static constexpr int kMaxPerCall = 64;

 private:
  int takePending() {
    int out = m_pending;
    if (out > kMaxPerCall) out = kMaxPerCall;
    if (out < -kMaxPerCall) out = -kMaxPerCall;
    m_pending -= out;
    // Cap what remains, not just what left. A residual larger than one more
    // pass' worth is discarded outright rather than carried forward - see
    // kMaxPerCall's comment for why that is safe.
    if (m_pending > kMaxPerCall) m_pending = kMaxPerCall;
    if (m_pending < -kMaxPerCall) m_pending = -kMaxPerCall;
    return out;
  }

  static const int64_t kGlitchLimit = 16384;

  int m_countsPerDetent;
  int64_t m_lastRaw;
  int m_residue;
  int m_pending;
  unsigned long m_glitchDrops;
};

#endif
