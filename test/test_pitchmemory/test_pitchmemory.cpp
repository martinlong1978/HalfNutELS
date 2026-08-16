// Pins the per-(mode,unit) pitch memory described in docs/ux-redesign.md
// Sec. 4 ("MODE") and Sec. 6 ("Units" menu tile note):
//
//   - There are four remembered pitch-select slots: {FEED,THREAD} x
//     {METRIC,IMPERIAL}. FM_THREAD_REVERSE shares the THREAD slot for the
//     current unit (same pitch tables, opposite direction).
//   - Changing feed mode (IncFeedMode()) and changing unit (setUnitMode())
//     must each PRESERVE the index for the slot being returned to, instead
//     of resetting to the mode/unit default every time.
//   - setUnitMode() must never leave getFeedSelect() out of range for the
//     newly-selected pitch array, and getCurrentFeedPitch() must stay sane
//     (non-zero) across a unit change.
//
// Today IncFeedMode() ends with setFeedSelect(-1), which snaps back to the
// mode/unit default on every call - so the memory tests below are expected
// to FAIL against current code. setUnitMode() is a bare assignment to
// m_unitMode alone; see the bounds test at the bottom for how that interacts
// with array sizes.
//
// GlobalState is a process-wide singleton that persists across tests in this
// binary (see test/test_feedmode/test_feedmode.cpp for the same caveat) -
// every test here drives the mode/unit to a known state before asserting.
#include <gmock/gmock.h>

#include "config.h"
#include "globalstate.h"

namespace {

// Bounded so a broken cycle fails the next ASSERT_EQ instead of hanging.
GlobalState* toMode(GlobalState* gs, GlobalFeedMode target) {
  for (int i = 0; i < 8 && gs->getFeedMode() != target; i++) {
    gs->IncFeedMode();
  }
  return gs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Concrete scenario from the task: step feed pitch off default, switch to
// thread and step it somewhere else, switch back to feed - the original feed
// index must be restored (not reset to the mode default).
// ---------------------------------------------------------------------------
TEST(PitchMemory, FeedIndexPreservedAcrossModeSwitchUsingStepFunctions) {
  GlobalState* gs = GlobalState::getInstance();
  gs->setUnitMode(METRIC);
  toMode(gs, FM_FEED);

  gs->setFeedSelect(DEFAULT_METRIC_FEED_PITCH_IDX);
  gs->nextFeedPitch();
  gs->nextFeedPitch();
  int feedIdx = gs->getFeedSelect();
  ASSERT_NE(feedIdx, DEFAULT_METRIC_FEED_PITCH_IDX)
      << "test setup: pitch should have moved off the default";

  toMode(gs, FM_THREAD);
  gs->setFeedSelect(DEFAULT_METRIC_THREAD_PITCH_IDX);
  gs->prevFeedPitch();
  gs->prevFeedPitch();
  int threadIdx = gs->getFeedSelect();
  ASSERT_NE(threadIdx, DEFAULT_METRIC_THREAD_PITCH_IDX)
      << "test setup: thread pitch should have moved off the default";

  toMode(gs, FM_FEED);
  EXPECT_EQ(gs->getFeedSelect(), feedIdx)
      << "returning to FEED should restore the remembered feed index, not "
         "reset to the default";
}

// ---------------------------------------------------------------------------
// Full four-slot round trip: METRIC/FEED, METRIC/THREAD, IMPERIAL/FEED,
// IMPERIAL/THREAD each get a distinctive index; THREAD_REVERSE is checked
// against the THREAD slot for the current unit; then every slot is
// re-visited and must still hold its own remembered index, independent of
// everything that happened to the other three slots in between.
// ---------------------------------------------------------------------------
TEST(PitchMemory, AllFourModeUnitSlotsAreIndependent) {
  GlobalState* gs = GlobalState::getInstance();

  gs->setUnitMode(METRIC);
  toMode(gs, FM_FEED);
  gs->setFeedSelect(3);
  const int metricFeedIdx = gs->getFeedSelect();
  ASSERT_EQ(metricFeedIdx, 3);

  toMode(gs, FM_THREAD);
  gs->setFeedSelect(15);
  const int metricThreadIdx = gs->getFeedSelect();
  ASSERT_EQ(metricThreadIdx, 15);

  gs->setUnitMode(IMPERIAL);
  toMode(gs, FM_FEED);
  gs->setFeedSelect(7);
  const int imperialFeedIdx = gs->getFeedSelect();
  ASSERT_EQ(imperialFeedIdx, 7);

  toMode(gs, FM_THREAD);
  gs->setFeedSelect(11);
  const int imperialThreadIdx = gs->getFeedSelect();
  ASSERT_EQ(imperialThreadIdx, 11);

  // Re-visit IMPERIAL/FEED without ever leaving IMPERIAL.
  toMode(gs, FM_FEED);
  EXPECT_EQ(gs->getFeedSelect(), imperialFeedIdx)
      << "IMPERIAL/FEED slot should still hold its own remembered index";

  // Cross back to METRIC/FEED.
  gs->setUnitMode(METRIC);
  toMode(gs, FM_FEED);
  EXPECT_EQ(gs->getFeedSelect(), metricFeedIdx)
      << "METRIC/FEED slot should still hold its own remembered index";

  // METRIC/THREAD.
  toMode(gs, FM_THREAD);
  EXPECT_EQ(gs->getFeedSelect(), metricThreadIdx)
      << "METRIC/THREAD slot should still hold its own remembered index";

  // THREAD_REVERSE shares the THREAD slot for the current unit (METRIC).
  toMode(gs, FM_THREAD_REVERSE);
  EXPECT_EQ(gs->getFeedSelect(), metricThreadIdx)
      << "THREAD_REVERSE should share METRIC/THREAD's remembered index";

  // Finally IMPERIAL/THREAD.
  gs->setUnitMode(IMPERIAL);
  toMode(gs, FM_THREAD);
  EXPECT_EQ(gs->getFeedSelect(), imperialThreadIdx)
      << "IMPERIAL/THREAD slot should still hold its own remembered index";

  // And IMPERIAL's THREAD_REVERSE shares IMPERIAL/THREAD's slot too.
  toMode(gs, FM_THREAD_REVERSE);
  EXPECT_EQ(gs->getFeedSelect(), imperialThreadIdx)
      << "THREAD_REVERSE should share IMPERIAL/THREAD's remembered index";
}

// ---------------------------------------------------------------------------
// Item 3: setUnitMode() must not leave a stale/out-of-range index.
//
// NOTE: with today's config.h, every pitch table (feedPitchMetric,
// feedPitchImperial, threadPitchMetric, threadPitchImperial) happens to have
// exactly 20 entries, so a raw carried-over index can never actually go
// out-of-bounds today and this test is expected to PASS against current
// code. It is still meaningful: setUnitMode() is documented as "a bare
// assignment" that does not even look at m_feedSelect, so this only holds by
// coincidence of today's table sizes, not by design - it pins the invariant
// so a future change to any one table's length (or the per-slot-memory
// rework in the tests above) can't silently produce an out-of-range
// getFeedSelect() or a garbage getCurrentFeedPitch().
// ---------------------------------------------------------------------------
TEST(PitchMemory, SetUnitModeLeavesFeedSelectInBoundsWithSanePitch) {
  GlobalState* gs = GlobalState::getInstance();

  for (GlobalFeedMode mode : {FM_FEED, FM_THREAD, FM_THREAD_REVERSE}) {
    toMode(gs, mode);
    for (GlobalUnitMode unit : {METRIC, IMPERIAL, METRIC, IMPERIAL}) {
      gs->setUnitMode(unit);

      const bool thread = (mode == FM_THREAD || mode == FM_THREAD_REVERSE);
      const int size = thread
                            ? (unit == METRIC ? (int)ARRAY_SIZE(threadPitchMetric)
                                               : (int)ARRAY_SIZE(threadPitchImperial))
                            : (unit == METRIC ? (int)ARRAY_SIZE(feedPitchMetric)
                                               : (int)ARRAY_SIZE(feedPitchImperial));

      int idx = gs->getFeedSelect();
      EXPECT_GE(idx, 0) << "feed select must not be negative after a unit "
                            "change (mode="
                         << mode << ", unit=" << unit << ")";
      EXPECT_LT(idx, size) << "feed select must be in range for the "
                               "newly-selected array (mode="
                            << mode << ", unit=" << unit << ")";

      float pitch = gs->getCurrentFeedPitch();
      EXPECT_NE(pitch, 0.0f) << "pitch must be non-zero after a unit change "
                                 "(mode="
                              << mode << ", unit=" << unit << ")";
    }
  }
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
