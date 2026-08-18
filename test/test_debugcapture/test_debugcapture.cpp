// Host tests for the motion-trace capture (lib/global_state/debugcapture.*).
//
// This is the instrument the thread-cutting regression is being hunted with, so
// the properties that decide whether a trace is TRUSTWORTHY are the ones pinned
// here:
//   * the disabled path really is inert (no buffer, no cursor to dereference);
//   * the decimator records a direction reversal on the instant it happens,
//     not on the next periodic tick, because the reversal timestamps are the
//     whole point of the capture;
//   * a full buffer STOPS, it does not wrap over the evidence;
//   * the CSV column order matches the header the analysis script parses.
#include <gtest/gtest.h>

#include <stdlib.h>
#include <string.h>

// debugcapture.h is all this file actually needs. config.h and globalstate.h
// are included for PlatformIO's library dependency finder, which only chains
// so far: without them lib/config is discovered but lib/dro (which
// latheconfig.h includes) is not, and the build fails on `dro.h: No such file`.
// Every other suite here carries the same pair for the same reason.
#include "config.h"
#include "globalstate.h"

#include <map>
#include <vector>

#include "debugcapture.h"

namespace {

// --- Allocator seam ---------------------------------------------------------
// Lets the size ladder and the out-of-memory path be driven deterministically;
// a real malloc never fails at these sizes on the host.
// Two independent models, because the ladder now answers two questions.
// g_failFirstN fails the first N calls outright ("this allocation cannot be
// served"). g_budgetBytes models a FIXED-SIZE heap: a call succeeds only while
// the outstanding total still fits. The second is what reproduces the failure
// seen on the device - a trace that fits, followed by an upload that does not.
size_t g_failFirstN = 0;   // fail this many allocations, then succeed
size_t g_budgetBytes = 0;  // 0 = unlimited
size_t g_outstanding = 0;
size_t g_allocCalls = 0;
size_t g_freeCalls = 0;
size_t g_lastAllocBytes = 0;
std::vector<size_t> g_allocSizes;
std::map<void*, size_t> g_liveSizes;

void* testAlloc(size_t bytes) {
  g_allocCalls++;
  g_lastAllocBytes = bytes;
  g_allocSizes.push_back(bytes);
  if (g_allocCalls <= g_failFirstN) {
    return 0;
  }
  if (g_budgetBytes != 0 && g_outstanding + bytes > g_budgetBytes) {
    return 0;
  }
  void* p = malloc(bytes);
  if (p != 0) {
    g_outstanding += bytes;
    g_liveSizes[p] = bytes;
  }
  return p;
}

void testFree(void* p) {
  g_freeCalls++;
  std::map<void*, size_t>::iterator it = g_liveSizes.find(p);
  if (it != g_liveSizes.end()) {
    g_outstanding -= it->second;
    g_liveSizes.erase(it);
  }
  free(p);
}

class DebugCaptureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_failFirstN = 0;
    g_budgetBytes = 0;
    g_outstanding = 0;
    g_allocSizes.clear();
    g_liveSizes.clear();
    g_allocCalls = 0;
    g_freeCalls = 0;
    g_lastAllocBytes = 0;
    DebugCapture::setAllocatorForTest(testAlloc, testFree);
  }
  void TearDown() override {
    DebugCapture::setAllocatorForTest(0, 0);  // back to malloc/free
  }
};

// Fills one slot with recognisable values and commits it, in the same order
// Leadscrew::update() does: note the iteration first (which is what measures
// the loop gap and the spindle delta), then fill, then commit.
void writeSample(DebugCapture& c, uint32_t now, int dir, float err) {
  c.noteIteration(now, 0);
  DebugData* s = c.slot();
  s->tm = c.relativeMicros(now);
  s->positionError = err;
  s->positionErrorRaw = err;
  s->pulsesToTargetSpeed = 0.0f;
  s->m_currentPosition = 0;
  s->m_currentDirection = dir;
  s->m_expectedPosition = 0.0f;
  s->m_leadscrewSpeed = 0.0f;
  s->m_targetSpeed = 0.0f;
  s->m_speedDif = 0.0f;
  s->m_timeToTarget = 0.0f;
  s->loopGapUs = c.peakGapUs();
  s->spindleDelta = c.peakSpindleDelta();
  c.commit(now, dir);
}

// ===========================================================================
// 1. The disabled path
// ===========================================================================

TEST_F(DebugCaptureTest, StartsOffWithNoBufferAndNoCursor) {
  DebugCapture c;
  EXPECT_FALSE(c.recording());
  EXPECT_EQ(DBG_OFF, c.state());
  EXPECT_EQ(0, c.count());
  EXPECT_EQ(0, c.capacity());
  // The cursor is null, and recording() is false, so the two capture sites in
  // Leadscrew::update() never reach it. THE OLD CODE HAD EXACTLY THIS PAIR OF
  // POINTERS AND NEVER ALLOCATED THEM - turning debug mode on wrote through
  // null from the spindle hot loop.
  EXPECT_EQ((DebugData*)0, c.slot());
  EXPECT_EQ((const DebugData*)0, c.data());
  EXPECT_EQ(0u, g_allocCalls) << "constructing a capture must not allocate";
}

TEST_F(DebugCaptureTest, DiscardOnAFreshCaptureIsANoOp) {
  DebugCapture c;
  c.discard();
  EXPECT_FALSE(c.recording());
  EXPECT_EQ(DBG_OFF, c.state());
  EXPECT_EQ(0u, g_freeCalls);
}

// ===========================================================================
// 2. Arming, capacity and the size ladder
// ===========================================================================

TEST_F(DebugCaptureTest, ArmAllocatesTheFullWindowAndStartsRecording) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(1000));
  EXPECT_TRUE(c.recording());
  EXPECT_EQ(DBG_RECORDING, c.state());
  EXPECT_EQ(kWantSamples, c.capacity());
  EXPECT_EQ(0, c.count());
  EXPECT_NE((DebugData*)0, c.slot());
  ASSERT_EQ(2u, g_allocCalls) << "the trace, then the upload reserve";
  EXPECT_EQ((size_t)kWantSamples * sizeof(DebugData), g_allocSizes[0]);
  EXPECT_EQ(kUploadReserveBytes, g_allocSizes[1]);
  EXPECT_TRUE(c.hasUploadReserve());
  c.discard();
}

TEST_F(DebugCaptureTest, TheWindowSpansSeveralGlitches) {
  // The sizing decision, made falsifiable: the reported glitch recurs every
  // 2-10 s, so a buffer that spans only a few seconds is useless. capacity x
  // interval must comfortably contain several glitches.
  const double seconds =
      (double)kWantSamples * (double)kSampleIntervalMicros / 1e6;
  EXPECT_GE(seconds, 30.0) << "the capture window must span several glitches";
  EXPECT_NEAR(50.0, seconds, 0.5);
}

TEST_F(DebugCaptureTest, TheBufferStillFitsTheDramBudget) {
  // The starvation pair was paid for out of window length, NOT out of the heap
  // the upload needs: the trace stays resident while it is being sent, next to
  // a TLS session for an https sink. Pins the footprint so a future field
  // cannot quietly grow the allocation instead.
  EXPECT_EQ((size_t)52, sizeof(DebugData));
  const size_t bytes = (size_t)kWantSamples * sizeof(DebugData);
  EXPECT_LE(bytes, (size_t)110000) << "the trace must leave room for the upload";
}

TEST_F(DebugCaptureTest, AFragmentedHeapCostsWindowLengthNotTheCapture) {
  // First allocation fails: it must step DOWN a size rather than give up, so
  // that a fragmented heap still yields a usable trace and still leaves room
  // for the upload's TLS session.
  g_failFirstN = 1;
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  EXPECT_TRUE(c.recording());
  EXPECT_LT(c.capacity(), kWantSamples);
  EXPECT_GE(c.capacity(), kMinSamples);
  c.discard();
}

TEST_F(DebugCaptureTest, EveryAllocationFailingLeavesItOffAndSaysSo) {
  g_failFirstN = 100;  // more than the ladder has rungs
  DebugCapture c;
  EXPECT_FALSE(c.arm(0));
  EXPECT_FALSE(c.recording()) << "a failed arm must not leave the hot loop "
                                 "writing through a null cursor";
  EXPECT_EQ(DBG_NOMEM, c.state());
  EXPECT_EQ(0, c.capacity());
  EXPECT_EQ((DebugData*)0, c.slot());
}

// --- The upload reserve ----------------------------------------------------
//
// REGRESSION: the device recorded a 2000-sample trace and could then never
// send it. Boot heap measured free=226,584 largest=110,580: the 104,000-byte
// trace fit inside the largest block with ~6.5 KB behind it, the ladder was
// satisfied, and xTaskCreatePinnedToCore then failed to find a contiguous
// 20 KB stack. It surfaced only as "send failed", which reads as a network
// fault. Total free heap was never the constraint - contiguity was.

TEST_F(DebugCaptureTest, LadderDropsARungWhenTheUploadWouldNotFitBehindTheTrace) {
  // The measured block, exactly. The top rung fits the trace and strands the
  // upload; a correct ladder must refuse it.
  g_budgetBytes = 110580;
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));

  EXPECT_LT(c.capacity(), kWantSamples)
      << "the rung that strands the upload must be rejected";
  EXPECT_TRUE(c.hasUploadReserve())
      << "a trace that cannot be sent is worthless";
  EXPECT_LE((size_t)c.capacity() * sizeof(DebugData) + kUploadReserveBytes,
            g_budgetBytes)
      << "trace and upload reserve must BOTH fit in the real heap";
  EXPECT_GE(c.capacity(), kMinSamples) << "still a usable window";
  c.discard();
}

TEST_F(DebugCaptureTest, ARejectedRungGivesItsTraceBackBeforeTryingTheNext) {
  g_budgetBytes = 110580;
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  // Whatever the ladder tried and abandoned must not still be held, or the
  // next rung is measured against a heap the failed attempt is squatting in.
  EXPECT_EQ((size_t)c.capacity() * sizeof(DebugData) + kUploadReserveBytes,
            g_outstanding)
      << "only the accepted trace and its reserve remain outstanding";
  c.discard();
  EXPECT_EQ(0u, g_outstanding) << "discard leaves nothing behind";
}

TEST_F(DebugCaptureTest, ArmFailsRatherThanRecordAnUnsendableTrace) {
  // Enough for the smallest window but not for the upload behind it. Recording
  // anyway would produce a trace that can only ever fail to send.
  g_budgetBytes = (size_t)kMinSamples * sizeof(DebugData) + 1024;
  DebugCapture c;
  EXPECT_FALSE(c.arm(0));
  EXPECT_EQ(DBG_NOMEM, c.state());
  EXPECT_FALSE(c.recording());
  EXPECT_EQ(0u, g_outstanding) << "a failed arm must hold nothing";
}

TEST_F(DebugCaptureTest, ReleasingTheUploadReserveIsIdempotent) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  c.releaseUploadReserve();
  EXPECT_FALSE(c.hasUploadReserve());
  EXPECT_EQ(1u, g_freeCalls);
  // The retry path after a failed POST reaches here again; it must not
  // double-free the block it already handed back.
  c.releaseUploadReserve();
  EXPECT_EQ(1u, g_freeCalls) << "second call is a no-op";
  EXPECT_TRUE(c.recording()) << "handing back the reserve does not stop the trace";
  c.discard();
}

TEST_F(DebugCaptureTest, TheTraceSurvivesHandingBackTheUploadReserve) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 1.5f);
  c.releaseUploadReserve();
  ASSERT_EQ(1, c.count());
  ASSERT_NE((const DebugData*)0, c.data());
  EXPECT_FLOAT_EQ(1.5f, c.data()[0].positionError)
      << "the payload must still be intact when the upload reads it";
  c.discard();
}

TEST_F(DebugCaptureTest, ReArmingRestartsRatherThanLeaking) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);
  ASSERT_EQ(1, c.count());

  ASSERT_TRUE(c.arm(5000));
  EXPECT_EQ(0, c.count()) << "a second arm starts a new trace";
  EXPECT_EQ(4u, g_allocCalls) << "trace + reserve, twice";
  EXPECT_EQ(2u, g_freeCalls) << "the first buffer AND its reserve must be freed";
  c.discard();
}

TEST_F(DebugCaptureTest, DiscardFreesAndStopsTheHotLoopFirst) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  c.discard();
  EXPECT_FALSE(c.recording());
  EXPECT_EQ((DebugData*)0, c.slot());
  EXPECT_EQ(DBG_OFF, c.state());
  EXPECT_EQ(2u, g_freeCalls) << "the trace and the upload reserve";
  EXPECT_FALSE(c.hasUploadReserve());
}

// ===========================================================================
// 3. The decimator (DebugCapture::due) - the heart of the instrument
// ===========================================================================

TEST_F(DebugCaptureTest, TheFirstUpdateAfterArmingIsRecorded) {
  // Seeded an interval in the past on purpose: a trace that opened with a
  // 25 ms hole would lose the moment the operator engaged the feed.
  DebugCapture c;
  ASSERT_TRUE(c.arm(1000000));
  EXPECT_TRUE(c.due(1000000, 0));
  c.discard();
}

TEST_F(DebugCaptureTest, PeriodicSamplingIsUniformInTimeNotInPulses) {
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);

  // Same direction: nothing until a full interval of REAL TIME has passed,
  // however many pulses went out in between.
  EXPECT_FALSE(c.due(1, 1));
  EXPECT_FALSE(c.due((uint32_t)kSampleIntervalMicros - 1, 1));
  EXPECT_TRUE(c.due((uint32_t)kSampleIntervalMicros, 1));
  EXPECT_TRUE(c.due((uint32_t)kSampleIntervalMicros * 4, 1));
  c.discard();
}

TEST_F(DebugCaptureTest, ADirectionReversalIsRecordedImmediately) {
  // THE POINT OF THE WHOLE SCHEME. The reported symptom is a ~180 degree
  // reversal every 2-10 s; its EDGE timestamps are what turn that into an
  // interval and a trigger condition. A purely periodic decimator would place
  // the edge anywhere in a 25 ms window.
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);

  EXPECT_FALSE(c.due(500, 1)) << "same direction, well inside the interval";
  EXPECT_TRUE(c.due(1000, -1)) << "the reversal must not wait for the tick";
  c.discard();
}

TEST_F(DebugCaptureTest, DirectionChatterIsRateLimitedNotUnbounded) {
  // A direction that flips on every pulse must not be able to sample at the
  // pulse rate; the floor is kBurstGapMicros. (It IS allowed to consume the
  // buffer quickly - a trace full of chatter is a trace of the bug - but at a
  // bounded rate, so it takes seconds rather than milliseconds.)
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);
  EXPECT_FALSE(c.due((uint32_t)kBurstGapMicros - 1, -1));
  EXPECT_TRUE(c.due((uint32_t)kBurstGapMicros, -1));
  c.discard();
}

// ---------------------------------------------------------------------------
// The starvation pair. These are the tests that decide whether the capture can
// answer the question at all: the lathe is the only place the bug reproduces
// and nobody can read a serial line there, so a stall that does not reach the
// uploaded trace is a stall nobody will ever see.
// ---------------------------------------------------------------------------

TEST_F(DebugCaptureTest, AStallForcesItsOwnSampleInsteadOfWaitingForTheTick) {
  // A 2 ms starvation gap falls between two 25 ms periodic samples far more
  // often than not. It has to force a sample, or the record will show the
  // CONSEQUENCE (an error spike) with no sign of the CAUSE.
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);

  // An ordinary iteration 500 us later: nothing due yet.
  c.noteIteration(500, 0);
  EXPECT_FALSE(c.due(500, 1));

  // Now the loop misses iterations for kStallGapMicros.
  const uint32_t stallEnd = 500 + (uint32_t)kStallGapMicros;
  c.noteIteration(stallEnd, 0);
  EXPECT_TRUE(c.due(stallEnd, 1)) << "a stall must not wait for the next tick";
  EXPECT_EQ((int)kStallGapMicros, c.peakGapUs());
}

TEST_F(DebugCaptureTest, APiledUpSpindleDeltaForcesItsOwnSample) {
  // The other half of the hypothesis: counts pile up in the encoder while the
  // loop is not running, and the next consumePosition() returns them all at
  // once. That spike must be timestamped, not averaged away.
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);

  c.noteIteration(100, kSpindleDeltaTrigger - 1);
  EXPECT_FALSE(c.due(100, 1)) << "an ordinary delta is not an event";

  c.noteIteration(1100, kSpindleDeltaTrigger);
  EXPECT_TRUE(c.due(1100, 1));
  EXPECT_EQ(kSpindleDeltaTrigger, c.peakSpindleDelta());
}

TEST_F(DebugCaptureTest, TheStarvationPairIsPeakHeldSoDecimationCannotHideIt) {
  // Belt and braces behind the forced sample: the two fields report the WORST
  // the loop did since the previous row, not whatever the sampled iteration
  // happened to see. Without this a stall could be recorded as "0 us gap"
  // simply because the next-but-one iteration was the one that got sampled.
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  writeSample(c, 0, 1, 0.0f);

  c.noteIteration(50, 1);        // quiet
  c.noteIteration(3050, -40);    // a 3 ms gap and a pile-up
  c.noteIteration(3060, 0);      // quiet again - would erase both, unheld
  EXPECT_EQ(3000, c.peakGapUs());
  EXPECT_EQ(-40, c.peakSpindleDelta())
      << "the sign is kept: a backward pile-up is a different fault";
}

TEST_F(DebugCaptureTest, CommittingOpensAFreshPeakHoldWindow) {
  // Otherwise one stall would mark every subsequent row for the rest of the
  // trace, and the interval between stalls - the number that matters - would
  // be unrecoverable.
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  c.noteIteration(3000, 30);
  ASSERT_TRUE(c.disturbed());
  writeSample(c, 3000, 1, 0.0f);

  EXPECT_FALSE(c.disturbed()) << "the event must not latch across samples";
  EXPECT_EQ(0, c.peakGapUs());
  EXPECT_EQ(0, c.peakSpindleDelta());
  c.noteIteration(3100, 0);
  EXPECT_FALSE(c.due(3100, 1)) << "back to the ordinary periodic rate";
  c.discard();
}

TEST_F(DebugCaptureTest, TheFirstIterationAfterArmingIsNotASpuriousStall) {
  // m_lastIterationMicros is seeded at arm(). Left at zero it would measure the
  // gap from whenever the object was CONSTRUCTED - i.e. from boot - and report
  // a multi-minute "stall" on the first row of every capture.
  DebugCapture c;
  const uint32_t armedAt = 90 * 1000 * 1000;  // 90 s after boot
  ASSERT_TRUE(c.arm(armedAt));
  c.noteIteration(armedAt + 40, 0);
  EXPECT_EQ(40, c.peakGapUs());
  EXPECT_FALSE(c.disturbed());
  c.discard();
}

TEST_F(DebugCaptureTest, TheLoopGapIsCorrectAcrossTheMicrosRollover) {
  DebugCapture c;
  const uint32_t nearWrap = 0xFFFFFF00u;
  ASSERT_TRUE(c.arm(nearWrap));
  const uint32_t after = nearWrap + 3000;  // wraps
  ASSERT_LT(after, nearWrap);
  c.noteIteration(after, 0);
  EXPECT_EQ(3000, c.peakGapUs())
      << "a rollover must not be reported as a stall";
  c.discard();
}

TEST_F(DebugCaptureTest, TheDecimatorIsCorrectAcrossTheMicrosRollover) {
  // micros() is 32 bits on both the ESP32 and the host stub, so a capture can
  // straddle the ~71-minute wrap. Unsigned differences handle it; a signed
  // widening would produce one absurd interval that reads exactly like the
  // glitch being hunted.
  const uint32_t nearWrap = 0xFFFFFF00u;
  DebugCapture c;
  ASSERT_TRUE(c.arm(nearWrap));
  writeSample(c, nearWrap, 1, 0.0f);

  const uint32_t afterWrap = nearWrap + (uint32_t)kSampleIntervalMicros;
  ASSERT_LT(afterWrap, nearWrap) << "this test must actually cross the wrap";
  EXPECT_TRUE(c.due(afterWrap, 1));
  EXPECT_EQ((int32_t)kSampleIntervalMicros, c.relativeMicros(afterWrap));
  c.discard();
}

// ===========================================================================
// 4. Filling up
// ===========================================================================

TEST_F(DebugCaptureTest, AFullBufferStopsTheCaptureAndDoesNotWrap) {
  g_failFirstN = 3;  // force the smallest rung so this test stays quick
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  const int cap = c.capacity();
  ASSERT_EQ(kMinSamples, cap);

  const DebugData* base = c.data();
  for (int i = 0; i < cap; i++) {
    ASSERT_TRUE(c.recording()) << "stopped early at " << i;
    writeSample(c, (uint32_t)i * 1000u, 1, (float)i);
  }

  EXPECT_FALSE(c.recording()) << "a full buffer must stop recording";
  EXPECT_EQ(DBG_FULL, c.state());
  EXPECT_EQ(cap, c.count());
  // Not a ring: the FIRST sample is still the first one written. An interval
  // between glitches cannot be measured from a window that has discarded the
  // previous glitch.
  EXPECT_EQ(0.0f, base[0].positionError);
  EXPECT_EQ((float)(cap - 1), base[cap - 1].positionError);
  c.discard();
}

TEST_F(DebugCaptureTest, AFullCaptureIsReadyToSendAndSoIsAFailedOne) {
  g_failFirstN = 3;
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  EXPECT_FALSE(c.readyToSend()) << "nothing captured yet";
  for (int i = 0; i < c.capacity(); i++) {
    writeSample(c, (uint32_t)i * 1000u, 1, 0.0f);
  }
  EXPECT_TRUE(c.readyToSend());

  // A failed upload KEEPS the buffer so a retry does not need the cut re-run.
  c.setState(DBG_FAILED);
  EXPECT_TRUE(c.readyToSend());

  c.setState(DBG_SENDING);
  EXPECT_FALSE(c.readyToSend()) << "an upload already in flight is not ready";
  c.discard();
}

TEST_F(DebugCaptureTest, ReleaseHandsBackTheRamButKeepsTheCount) {
  g_failFirstN = 3;
  DebugCapture c;
  ASSERT_TRUE(c.arm(0));
  for (int i = 0; i < c.capacity(); i++) {
    writeSample(c, (uint32_t)i * 1000u, 1, 0.0f);
  }
  const int captured = c.count();

  c.release(DBG_SENT);
  EXPECT_EQ(2u, g_freeCalls) << "the trace and the upload reserve go back";
  EXPECT_EQ(DBG_SENT, c.state());
  EXPECT_EQ(captured, c.count()) << "the status line still reports the count";
  EXPECT_FALSE(c.readyToSend()) << "a released trace must not be sent twice";
  EXPECT_EQ((const DebugData*)0, c.data());
}

// ===========================================================================
// 5. The operator-visible status line
// ===========================================================================

TEST(CaptureStatus, EveryStateHasWordingThatFitsTheDisplayCache) {
  // The display caches this string in a 20-byte slot and TRUNCATES anything
  // longer - after which it never compares equal again and the label repaints
  // at 10 Hz forever. Every state, at the largest counts it can report.
  const int states[] = {DBG_OFF,     DBG_RECORDING, DBG_FULL, DBG_SENDING,
                        DBG_SENT,    DBG_FAILED,    DBG_NOMEM};
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
    char buf[64];
    formatCaptureStatus(buf, sizeof(buf), states[i], kWantSamples, kWantSamples);
    EXPECT_LT(strlen(buf), kCaptureStatusMax)
        << "state " << states[i] << " renders \"" << buf << "\"";
    EXPECT_GT(strlen(buf), 0u) << "state " << states[i] << " renders nothing";
  }
}

TEST(CaptureStatus, SaysWhatItIsDoing) {
  char buf[kCaptureStatusMax];
  formatCaptureStatus(buf, sizeof(buf), DBG_OFF, 0, 0);
  EXPECT_STREQ("DIAGNOSTICS", buf) << "no capture: just the screen's title";

  formatCaptureStatus(buf, sizeof(buf), DBG_RECORDING, 812, 2400);
  EXPECT_STREQ("REC 812/2400", buf);

  // The FULL wording is an instruction, because only the operator can act on
  // it: nothing is uploaded until the carriage is at rest.
  formatCaptureStatus(buf, sizeof(buf), DBG_FULL, 2400, 2400);
  EXPECT_STREQ("FULL: STOP TO SEND", buf);

  formatCaptureStatus(buf, sizeof(buf), DBG_SENT, 2400, 2400);
  EXPECT_STREQ("SENT 2400", buf);
}

// ===========================================================================
// 6. The wire format
// ===========================================================================

TEST(DebugCsv, HeaderIsTheOneTheAnalysisScriptParses) {
  // The starvation pair is APPENDED, so the original eleven columns keep their
  // positions and a capture taken before them still parses by index.
  EXPECT_STREQ(
      "time,posError,posErrorRaw,pulseToTarget,pos,expectedPos,speed,"
      "direction,targetSpeed,speedDiff,timeToTarget,loopGapUs,spindleDelta",
      debugCsvHeader());
}

TEST(DebugCsv, ColumnOrderFollowsTheHeaderNotTheStruct) {
  // `direction` is column 8 even though m_currentDirection is the sixth
  // member of DebugData. Getting this wrong would silently mislabel every
  // column after `pos` in every capture.
  DebugData d;
  d.tm = 1234567;
  d.positionError = 1.5f;
  d.positionErrorRaw = 2.5f;
  d.pulsesToTargetSpeed = 3.5f;
  d.m_currentPosition = 4;
  d.m_currentDirection = -1;
  d.m_expectedPosition = 6.5f;
  d.m_leadscrewSpeed = 7.5f;
  d.m_targetSpeed = 8.5f;
  d.m_speedDif = 9.5f;
  d.m_timeToTarget = 10.5f;
  d.loopGapUs = 8400;
  d.spindleDelta = -52;

  char row[kDebugCsvRowMax];
  const int n = formatDebugRow(row, sizeof(row), d);
  ASSERT_GT(n, 0);
  EXPECT_EQ((size_t)n, strlen(row));
  EXPECT_STREQ("1234567,1.5,2.5,3.5,4,6.5,7.5,-1,8.5,9.5,10.5,8400,-52", row);
  EXPECT_EQ((const char*)0, strchr(row, '\n'))
      << "rows carry no newline; the sender adds the separator";
}

TEST(DebugCsv, ATooSmallBufferIsRefusedRatherThanTruncated) {
  DebugData d;
  memset(&d, 0, sizeof(d));
  d.tm = 123456789;
  char row[8];
  EXPECT_LT(formatDebugRow(row, sizeof(row), d), 0);
}

TEST(DebugCsv, TheRowBufferSizeIsEnoughForExtremeValues) {
  DebugData d;
  d.tm = -2147483647;
  d.positionError = -1.2345678e30f;
  d.positionErrorRaw = 1.2345678e-30f;
  d.pulsesToTargetSpeed = -1.2345678e30f;
  d.m_currentPosition = -2147483647;
  d.m_currentDirection = -1;
  d.m_expectedPosition = -1.2345678e30f;
  d.m_leadscrewSpeed = -1.2345678e30f;
  d.m_targetSpeed = -1.2345678e30f;
  d.m_speedDif = -1.2345678e30f;
  d.m_timeToTarget = -1.2345678e30f;
  d.loopGapUs = 2147483647;
  d.spindleDelta = -2147483647;
  char row[kDebugCsvRowMax];
  EXPECT_GT(formatDebugRow(row, sizeof(row), d), 0);
}

// ===========================================================================
// 7. URL parsing (the configurable sink)
// ===========================================================================

TEST(ParseHttpUrl, PlainHttpDefaultsToPort80) {
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("http://hass.longhome.co.uk/capture", p));
  EXPECT_FALSE(p.secure);
  EXPECT_STREQ("hass.longhome.co.uk", p.host);
  EXPECT_EQ(80, p.port);
  EXPECT_STREQ("/capture", p.path);
}

TEST(ParseHttpUrl, HttpsDefaultsToPort443) {
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("https://example.com/x/y", p));
  EXPECT_TRUE(p.secure);
  EXPECT_EQ(443, p.port);
  EXPECT_STREQ("/x/y", p.path);
}

TEST(ParseHttpUrl, AnExplicitPortWins) {
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("http://hass.longhome.co.uk:8088/capture", p));
  EXPECT_STREQ("hass.longhome.co.uk", p.host);
  EXPECT_EQ(8088, p.port);
  EXPECT_STREQ("/capture", p.path);
}

TEST(ParseHttpUrl, NoPathBecomesRoot) {
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("http://10.0.0.5:8088", p));
  EXPECT_STREQ("10.0.0.5", p.host);
  EXPECT_EQ(8088, p.port);
  EXPECT_STREQ("/", p.path) << "the request line needs an origin-form path";
}

TEST(ParseHttpUrl, AQueryWithNoPathStillGetsALeadingSlash) {
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("http://host?a=b", p));
  EXPECT_STREQ("host", p.host);
  EXPECT_STREQ("/?a=b", p.path);
}

TEST(ParseHttpUrl, RejectsAnythingItCannotSendTo) {
  HttpUrlParts p;
  EXPECT_FALSE(parseHttpUrl(0, p));
  EXPECT_FALSE(parseHttpUrl("", p));
  EXPECT_FALSE(parseHttpUrl("hass.longhome.co.uk/capture", p)) << "no scheme";
  EXPECT_FALSE(parseHttpUrl("ftp://host/x", p));
  EXPECT_FALSE(parseHttpUrl("http:///capture", p)) << "empty host";
  EXPECT_FALSE(parseHttpUrl("http://host:/capture", p)) << "empty port";
  EXPECT_FALSE(parseHttpUrl("http://host:80x/capture", p));
  EXPECT_FALSE(parseHttpUrl("http://host:99999/capture", p));
  EXPECT_FALSE(parseHttpUrl("http://user@host/x", p)) << "userinfo unsupported";
}

TEST(ParseHttpUrl, AFailedParseLeavesNothingUsableBehind) {
  // A caller that ignores the return value must not be handed a stale host
  // from a previous parse.
  HttpUrlParts p;
  ASSERT_TRUE(parseHttpUrl("http://good.example/x", p));
  EXPECT_FALSE(parseHttpUrl("ftp://bad.example/x", p));
  EXPECT_STREQ("", p.host);
  EXPECT_EQ(0, p.port);
  EXPECT_STREQ("", p.path);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
