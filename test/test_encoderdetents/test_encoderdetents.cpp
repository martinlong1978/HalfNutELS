// Raw quadrature counts -> detents for the UI knob (lib/keyscan).
//
// These tests are the specification for the fix to the knob that skipped items
// and sometimes stepped backwards. The cause was a SINGLE EDGE decode, in which
// contact bounce accumulates as real motion because nothing ever decrements;
// the fix is a full quadrature decode, where a bounce cancels itself in the
// counter, plus the divide back down to detents that is pinned here.
//
// The bounce cases below feed the raw count sequences a bouncing contact
// actually produces under full quad, and assert what the operator sees. That is
// the whole point: a burst is a wobble around a position and nets to nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "encoderdetents.h"

#include <vector>

namespace {

const int kCountsPerDetent = 4;  // ELS_UI_ENCODER_COUNTS_PER_DETENT

// Drives the decoder the way src/keyarray.cpp does: hand it the free-running
// counter value, take whatever detents come back.
class Rig {
 public:
  Rig() : m_dec(kCountsPerDetent), m_raw(1000) { m_dec.reset(m_raw); }

  // Move the raw counter by `counts` and poll once, as one 100 ms pass would.
  int move(int counts) {
    m_raw += counts;
    return m_dec.update(m_raw);
  }

  // Move in single counts without polling in between - the counter runs on in
  // hardware while nothing is reading it - then poll once.
  int moveThenPoll(const std::vector<int>& counts) {
    for (size_t i = 0; i < counts.size(); i++) {
      m_raw += counts[i];
    }
    return m_dec.update(m_raw);
  }

  int poll() { return m_dec.update(m_raw); }

  EncoderDetents& dec() { return m_dec; }
  int64_t raw() const { return m_raw; }
  void jumpRawBy(int64_t v) { m_raw += v; }

 private:
  EncoderDetents m_dec;
  int64_t m_raw;
};

// --- The plain cases --------------------------------------------------------

TEST(EncoderDetents, FourCountsIsOneDetent) {
  Rig r;
  EXPECT_EQ(1, r.move(4));
  EXPECT_EQ(-1, r.move(-4));
}

TEST(EncoderDetents, NoMovementReportsNothing) {
  Rig r;
  EXPECT_EQ(0, r.poll());
  EXPECT_EQ(0, r.poll());
}

TEST(EncoderDetents, ASpinBetweenPollsArrivesWhole) {
  Rig r;
  // Twenty detents turned during one 100 ms display period.
  EXPECT_EQ(20, r.move(20 * kCountsPerDetent));
}

TEST(EncoderDetents, TwentyDetentsThereAndBackNetsToZero) {
  Rig r;
  int net = 0;
  for (int i = 0; i < 20; i++) net += r.move(kCountsPerDetent);
  for (int i = 0; i < 20; i++) net += r.move(-kCountsPerDetent);
  EXPECT_EQ(0, net);
  EXPECT_EQ(0, r.dec().pending());
}

// --- Sub-detent movement is held, not rounded -------------------------------

TEST(EncoderDetents, PartialTurnReportsNothingYet) {
  Rig r;
  EXPECT_EQ(0, r.move(1));
  EXPECT_EQ(0, r.move(1));
  EXPECT_EQ(0, r.move(1)) << "three of four counts is not a detent";
  EXPECT_EQ(1, r.move(1)) << "the fourth completes it";
}

TEST(EncoderDetents, KnobPushedOffADetentAndReleasedReportsNothing) {
  // The operator nudges the knob two counts and lets it snap back. This is the
  // case that must NOT produce a phantom step - it is what a resting knob on a
  // threshold does all day.
  Rig r;
  EXPECT_EQ(0, r.move(2));
  EXPECT_EQ(0, r.move(-2));
  EXPECT_EQ(0, r.dec().pending());
}

TEST(EncoderDetents, ResidueIsSymmetricAcrossZero) {
  // Truncation toward zero, so a counter-clockwise partial behaves exactly like
  // a clockwise one rather than rounding an extra step out of nowhere.
  Rig r;
  EXPECT_EQ(0, r.move(-3));
  EXPECT_EQ(-1, r.move(-1));
  EXPECT_EQ(0, r.move(3));
  EXPECT_EQ(1, r.move(1));
}

// --- Bounce: the actual bug -------------------------------------------------

TEST(EncoderDetents, FullQuadBounceBurstNetsToOneDetent) {
  // One detent CW, with A chattering five times on the way. Under full quad
  // every bounce edge is counted AND reversed, so the raw count wobbles around
  // the true position instead of running away from it.
  Rig r;
  std::vector<int> burst;
  burst.push_back(1);
  for (int i = 0; i < 5; i++) {
    burst.push_back(-1);
    burst.push_back(1);
  }
  burst.push_back(1);
  burst.push_back(1);
  burst.push_back(1);
  EXPECT_EQ(1, r.moveThenPoll(burst)) << "a bounce burst is still one detent";
  EXPECT_EQ(0, r.dec().pending());
}

TEST(EncoderDetents, StationaryKnobChatteringReportsNothing) {
  // A detent resting on a threshold: the contact chatters back and forth while
  // nobody is touching it. Under the old single-edge decode this accumulated
  // and walked the menu on its own.
  Rig r;
  int net = 0;
  for (int i = 0; i < 50; i++) {
    net += r.move(1);
    net += r.move(-1);
  }
  EXPECT_EQ(0, net);
}

TEST(EncoderDetents, BounceDoesNotReverseTheStep) {
  // The reversing symptom: it came from sampling B's level once, at the instant
  // A fell. Full quad has no such sample - direction is in the count itself -
  // so a burst inside a clockwise detent cannot come out counter-clockwise.
  Rig r;
  std::vector<int> burst;
  burst.push_back(1);
  burst.push_back(-1);
  burst.push_back(1);
  burst.push_back(-1);
  burst.push_back(1);
  burst.push_back(1);
  burst.push_back(1);
  burst.push_back(1);
  EXPECT_EQ(1, r.moveThenPoll(burst));
}

// --- The counter artefact ---------------------------------------------------

TEST(EncoderDetents, WildDeltaIsDroppedNotClamped) {
  // ESP32Encoder runs PCNT to +/-INT16 and accumulates the wrap in a limit ISR;
  // a read racing that ISR returns a boundary value. The old code clamped such
  // a delta to +/-64, which turns a counter artefact into 64 real menu steps.
  Rig r;
  r.jumpRawBy(32765);
  EXPECT_EQ(0, r.dec().update(r.raw()));
  EXPECT_EQ(1u, r.dec().glitchDrops());

  r.jumpRawBy(-32766);
  EXPECT_EQ(0, r.dec().update(r.raw()));
  EXPECT_EQ(2u, r.dec().glitchDrops());
}

TEST(EncoderDetents, RealMotionSurvivesAfterAGlitch) {
  Rig r;
  r.jumpRawBy(32765);
  EXPECT_EQ(0, r.dec().update(r.raw()));
  EXPECT_EQ(1, r.move(4)) << "the decoder re-bases on the glitched position";
  EXPECT_EQ(1u, r.dec().glitchDrops());
}

TEST(EncoderDetents, GlitchDropsStartAtZero) {
  Rig r;
  r.move(4);
  r.move(-4);
  EXPECT_EQ(0u, r.dec().glitchDrops());
}

// --- The bound ButtonPad's replay loop relies on ----------------------------

TEST(EncoderDetents, OneCallIsBoundedButNothingIsLost) {
  // ButtonPad replays one handleKey() per detent, so a single call must be
  // bounded. The excess is kept, not dropped - a bound must not cost a step.
  Rig r;
  const int detents = EncoderDetents::kMaxPerCall + 10;
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.move(detents * kCountsPerDetent));
  EXPECT_EQ(10, r.poll());
  EXPECT_EQ(0, r.poll());
}

TEST(EncoderDetents, TheBoundWorksInBothDirections) {
  Rig r;
  const int detents = EncoderDetents::kMaxPerCall + 3;
  EXPECT_EQ(-EncoderDetents::kMaxPerCall, r.move(-detents * kCountsPerDetent));
  EXPECT_EQ(-3, r.poll());
}

// --- GitHub issue #5: the backlog a bound-and-carry m_pending can leave -----
//
// update() itself has no notion of "the encoder is inhibited" - it is
// deliberately pure, with no Arduino, no UiContext, no idea the carriage
// exists (see the class comment at the top of this header), and that has not
// changed. What HAS changed is which layer is trusted to bound the backlog a
// directionally-biased noise burst can leave behind.
//
// The first attempt at issue #5 gated on the CALLER's knowledge of motion
// state (ButtonPad resetting the decoder while underPower()) and left this
// class's own carry-forward untouched - which is why the tests below used to
// pin `m_pending` growing arbitrarily large and draining kMaxPerCall a pass
// for as long as it took, "characterising" rather than fixing anything. That
// approach had a gap: the encoder is just as inert - and a biased burst just
// as possible - during the stepper alarm and in UiFocus::Stops, neither of
// which is "under power", so the backlog it was meant to prevent could still
// form.
//
// The fix instead caps the RESIDUAL left in m_pending once update() has taken
// its per-call share, to at most kMaxPerCall - see the class comment on
// kMaxPerCall. This needs no notion of "inhibited" at all: it does not ask
// why a detent went unclaimed, only how much can still be owed after one call
// returns. That is safe specifically because kMaxPerCall itself is already a
// bound no thumb can reach in a single 100 ms pass (kGlitchLimit's own
// comment: half the 16-bit range is 4096 detents, and a human is nowhere
// near even kMaxPerCall = 64) - so anything the cap discards was never a real
// step to begin with, and the discarding needs no context about the machine.
//
// The tests below now assert THAT invariant directly, and update the two that
// used to pin the old uncapped drain.
TEST(EncoderDetentsBacklogFlood,
     BacklogOnlyBeginsStrictlyAboveKMaxPerCallDetentsInASinglePass) {
  // The exact reachability boundary: a single 100 ms pass has to see MORE
  // than kMaxPerCall (64) whole detents - more than
  // kMaxPerCall * kCountsPerDetent = 256 raw counts - before anything is left
  // behind at all. Landing exactly on the bound leaves nothing.
  Rig r;
  EXPECT_EQ(EncoderDetents::kMaxPerCall,
            r.move(EncoderDetents::kMaxPerCall * kCountsPerDetent));
  EXPECT_EQ(0, r.dec().pending())
      << "exactly kMaxPerCall detents in one pass must not start a backlog";

  Rig r2;
  EXPECT_EQ(EncoderDetents::kMaxPerCall,
            r2.move((EncoderDetents::kMaxPerCall + 1) * kCountsPerDetent));
  EXPECT_EQ(1, r2.dec().pending())
      << "one detent past the bound is the first to survive into m_pending";
}

TEST(EncoderDetentsBacklogFlood, TheGlitchLimitItselfIsAcceptedNotDropped) {
  // kGlitchLimit is checked with a strict '>', so a delta of EXACTLY 16384
  // raw counts is accepted as real motion, not dropped as a glitch - this is
  // the single largest backlog one pass can ever create: 4096 whole detents.
  // kMaxPerCall (64) return immediately, as before; what changes is what is
  // left owing. Under the residual cap the other 4032 are not all kept - only
  // one further pass' worth (kMaxPerCall = 64) survives into m_pending, and
  // the rest is discarded on the spot. So the drain this burst can ever
  // produce is bounded at 128 detents total (64 now, 64 on the next poll) -
  // about 200 ms - not the 6.3 seconds an uncapped carry-forward would have
  // produced.
  Rig r;
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.move(16384));
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.dec().pending());
}

TEST(EncoderDetentsBacklogFlood,
     ABacklogDrainsAtMostOneFurtherKMaxPerCallPollRegardlessOfSize) {
  // The mechanism the issue called "floods... until it drains" is now bounded
  // to at most one further pass. N=200 whole detents in one burst: the first
  // call still returns kMaxPerCall (64, unchanged from before - see
  // OneCallIsBoundedButNothingIsLost) and used to leave 136 owing; the
  // residual cap knocks that down to kMaxPerCall (64) on the spot, so the
  // very next poll - with no further movement - pays out the rest in full and
  // the one after that is empty. Two passes end it, not four.
  Rig r;
  const int N = 200;
  ASSERT_GT(N, EncoderDetents::kMaxPerCall);
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.move(N * kCountsPerDetent));
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.dec().pending())
      << "the residual is capped, not the raw excess (136)";
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.poll());
  EXPECT_EQ(0, r.poll());
}

TEST(EncoderDetentsBacklogFlood,
     PendingNeverExceedsKMaxPerCallHoweverLargeTheAcceptedDelta) {
  // The invariant itself, named directly: whatever a single call accepts as
  // real motion, at most one further pass' worth may survive into m_pending.
  // Exercised at the largest delta update() will ever accept without dropping
  // it as a glitch (kGlitchLimit, exactly - see the test above) and again
  // after a second such burst stacked on top, so this is not merely true of
  // one call in isolation.
  Rig r;
  r.move(16384);
  EXPECT_LE(r.dec().pending(), EncoderDetents::kMaxPerCall);
  r.move(16384);
  EXPECT_LE(r.dec().pending(), EncoderDetents::kMaxPerCall);
}

TEST(EncoderDetentsBacklogFlood, ResetClearsAnyPendingBacklogCharacterization) {
  // NOT a new behaviour - reset() already zeroes m_pending (and m_residue),
  // rebasing m_lastRaw to the position handed in. It exists today for
  // KeyArray's construction-time seed, but nothing about its signature or
  // effect is specific to that use: calling reset(currentRawCount) at any
  // later time discards a backlog exactly as cleanly. A caller that already
  // knows the encoder is inhibited (ButtonPad has UiContext; this class
  // deliberately does not, see the class comment) can use this EXISTING
  // public method to drop a stale backlog without EncoderDetents needing to
  // learn anything about motion state. Recorded here so a future change does
  // not have to rediscover it.
  Rig r;
  EXPECT_EQ(EncoderDetents::kMaxPerCall, r.move(500 * kCountsPerDetent));
  ASSERT_GT(r.dec().pending(), 0);
  r.dec().reset(r.raw());
  EXPECT_EQ(0, r.dec().pending());
  EXPECT_EQ(0, r.poll());
}

// --- Construction -----------------------------------------------------------

TEST(EncoderDetents, ResetAdoptsThePositionWithoutReportingIt) {
  // The counter is free-running and is not zero when KeyArray is built. If the
  // decoder measured from zero, the first poll would replay the whole of it.
  EncoderDetents d(kCountsPerDetent);
  d.reset(123456);
  EXPECT_EQ(0, d.update(123456));
  EXPECT_EQ(1, d.update(123460));
}

TEST(EncoderDetents, HalfDetentPartsAreSupported) {
  // Not the board's encoder, but the divisor is a board property and a
  // half-detent part would set it to two.
  EncoderDetents d(2);
  d.reset(0);
  EXPECT_EQ(1, d.update(2));
  EXPECT_EQ(0, d.update(3));
  EXPECT_EQ(1, d.update(4));
}

}  // namespace

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
