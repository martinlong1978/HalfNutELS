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
// SINGLETON CAVEAT. GlobalState is a process-wide singleton that persists for
// the life of this binary, so nothing here gets fresh state. Two rules follow
// and both matter:
//   1. Every test drives mode AND unit to a known state before asserting -
//      never assume what the previous test left behind.
//   2. Constructor behaviour can only be observed by the FIRST test that
//      touches the singleton, which is why AAA_ConstructorDefaults is
//      deliberately the first TEST in this file. googletest runs suites in
//      order of first registration, so do not move it, and do not add a test
//      above it. (It would also be defeated by --gtest_shuffle; the native env
//      does not use it.)
//
// EXPECTATION SOURCE. Indices and bounds below are derived from ARRAY_SIZE of
// the relevant pitch table, never hardcoded. That is the whole point: these
// tests must break when a pitch table changes length and the code stops
// coping, not pass by coincidence of today's four 20-entry tables. Verified by
// mutation - shortening threadPitchImperial and re-running is what caught the
// original bounds test being decorative.
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

// The pitch table GlobalState will bounds-check against for a (mode, unit)
// pair - mirrors GlobalState::getCurrentFeedSelectArraySize(), which is
// protected. FM_THREAD_REVERSE shares FM_THREAD's table.
int arraySizeFor(GlobalFeedMode mode, GlobalUnitMode unit) {
  const bool thread = (mode == FM_THREAD || mode == FM_THREAD_REVERSE);
  if (unit == METRIC) {
    return thread ? (int)ARRAY_SIZE(threadPitchMetric)
                  : (int)ARRAY_SIZE(feedPitchMetric);
  }
  return thread ? (int)ARRAY_SIZE(threadPitchImperial)
                : (int)ARRAY_SIZE(feedPitchImperial);
}

int defaultIdxFor(GlobalFeedMode mode, GlobalUnitMode unit) {
  const bool thread = (mode == FM_THREAD || mode == FM_THREAD_REVERSE);
  if (unit == METRIC) {
    return thread ? DEFAULT_METRIC_THREAD_PITCH_IDX
                  : DEFAULT_METRIC_FEED_PITCH_IDX;
  }
  return thread ? DEFAULT_IMPERIAL_THREAD_PITCH_IDX
                : DEFAULT_IMPERIAL_FEED_PITCH_IDX;
}

// Put the singleton in an unambiguous (unit, mode) state. Order matters:
// setUnitMode() and IncFeedMode() both restore through setFeedSelect(), so the
// mode is settled last and the caller can then set the index it wants.
void toState(GlobalState* gs, GlobalUnitMode unit, GlobalFeedMode mode) {
  gs->setUnitMode(unit);
  toMode(gs, mode);
  ASSERT_EQ(gs->getUnitMode(), unit);
  ASSERT_EQ(gs->getFeedMode(), mode);
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor initialisation. CLAUDE.md records shipped bugs caused by
// heap-allocated objects (GlobalState is `new`ed) with members left holding
// heap garbage, so the constructed state is worth pinning explicitly - all the
// more since setFeedSelect() now READS m_feedMode/m_unitMode and WRITES
// m_pitchMemory, making the initialisation ORDER load-bearing.
//
// MUST STAY FIRST IN THIS FILE - see the singleton caveat at the top.
// ---------------------------------------------------------------------------
TEST(AAA_ConstructorDefaults, FeedSelectAndJogStartInBoundsForTheConfiguredDefaults) {
  GlobalState* gs = GlobalState::getInstance();

  // The configured defaults, not a hardcoded METRIC/FEED assumption.
  EXPECT_EQ(gs->getFeedMode(), DEFAULT_FEED_MODE);
  EXPECT_EQ(gs->getUnitMode(), DEFAULT_UNIT_MODE);

  const int size = arraySizeFor(DEFAULT_FEED_MODE, DEFAULT_UNIT_MODE);
  EXPECT_GE(gs->getFeedSelect(), 0);
  EXPECT_LT(gs->getFeedSelect(), size);
  // Must be the default for the slot the default mode/unit actually names -
  // seeding from a different slot's default would also stamp that wrong value
  // into m_pitchMemory through setFeedSelect()'s write-back.
  EXPECT_EQ(gs->getFeedSelect(), defaultIdxFor(DEFAULT_FEED_MODE, DEFAULT_UNIT_MODE));
  EXPECT_NE(gs->getCurrentFeedPitch(), 0.0f);

  // Jog index must index jogSpeeds[] on the very first read, before any
  // inc/decJogSpeed() call has had a chance to clamp it.
  EXPECT_GE(gs->getJogIndex(), 0);
  EXPECT_LT(gs->getJogIndex(), (int)ARRAY_SIZE(jogSpeeds));
  EXPECT_FLOAT_EQ(gs->getJogSpeed(), jogSpeeds[gs->getJogIndex()]);

  // All four slots must be seeded, not just the one the default mode/unit
  // selects - an uninitialised slot only shows up on the first switch INTO it.
  // Visiting each slot must yield an in-range index without any set*() first.
  for (GlobalUnitMode unit : {METRIC, IMPERIAL}) {
    for (GlobalFeedMode mode : {FM_FEED, FM_THREAD}) {
      gs->setUnitMode(unit);
      toMode(gs, mode);
      EXPECT_EQ(gs->getFeedSelect(), defaultIdxFor(mode, unit))
          << "slot (unit=" << unit << ", mode=" << mode
          << ") was not seeded with its configured default";
    }
  }
}

// ---------------------------------------------------------------------------
// Concrete scenario from the task: step feed pitch off default, switch to
// thread and step it somewhere else, switch back to feed - the original feed
// index must be restored (not reset to the mode default).
// ---------------------------------------------------------------------------
TEST(PitchMemory, FeedIndexPreservedAcrossModeSwitchUsingStepFunctions) {
  GlobalState* gs = GlobalState::getInstance();
  toState(gs, METRIC, FM_FEED);

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
//
// The four indices are derived from each table's own length (top, top-1,
// top-2, top-3) rather than hardcoded, so that shortening any one table makes
// this test exercise the PRODUCT, instead of failing on its own unsatisfiable
// setup. They stay mutually distinct, which is what makes slot cross-talk
// visible.
// ---------------------------------------------------------------------------
TEST(PitchMemory, AllFourModeUnitSlotsAreIndependent) {
  GlobalState* gs = GlobalState::getInstance();

  const int metricFeedIdx = arraySizeFor(FM_FEED, METRIC) - 1;
  const int metricThreadIdx = arraySizeFor(FM_THREAD, METRIC) - 2;
  const int imperialFeedIdx = arraySizeFor(FM_FEED, IMPERIAL) - 3;
  const int imperialThreadIdx = arraySizeFor(FM_THREAD, IMPERIAL) - 4;
  ASSERT_GE(metricFeedIdx, 0);
  ASSERT_GE(metricThreadIdx, 0);
  ASSERT_GE(imperialFeedIdx, 0);
  ASSERT_GE(imperialThreadIdx, 0);

  toState(gs, METRIC, FM_FEED);
  gs->setFeedSelect(metricFeedIdx);
  ASSERT_EQ(gs->getFeedSelect(), metricFeedIdx);

  toMode(gs, FM_THREAD);
  gs->setFeedSelect(metricThreadIdx);
  ASSERT_EQ(gs->getFeedSelect(), metricThreadIdx);

  gs->setUnitMode(IMPERIAL);
  toMode(gs, FM_FEED);
  gs->setFeedSelect(imperialFeedIdx);
  ASSERT_EQ(gs->getFeedSelect(), imperialFeedIdx);

  toMode(gs, FM_THREAD);
  gs->setFeedSelect(imperialThreadIdx);
  ASSERT_EQ(gs->getFeedSelect(), imperialThreadIdx);

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
// THREAD_REVERSE must share THREAD's slot in the WRITE direction too - the
// round-trip test above only ever reads through THREAD_REVERSE. An index set
// while in reverse has to be what plain THREAD comes back to.
// ---------------------------------------------------------------------------
TEST(PitchMemory, ThreadReverseWritesThroughToTheSharedThreadSlot) {
  GlobalState* gs = GlobalState::getInstance();
  toState(gs, METRIC, FM_THREAD_REVERSE);

  const int idx = arraySizeFor(FM_THREAD_REVERSE, METRIC) - 5;
  ASSERT_GE(idx, 0);
  gs->setFeedSelect(idx);
  ASSERT_EQ(gs->getFeedSelect(), idx);

  // Round the cycle back to plain THREAD (REVERSE -> FEED -> THREAD).
  toMode(gs, FM_THREAD);
  EXPECT_EQ(gs->getFeedSelect(), idx)
      << "an index set while in THREAD_REVERSE must be remembered by THREAD - "
         "they share one slot";

  // ...and stepping it in THREAD must be visible back in THREAD_REVERSE.
  gs->prevFeedPitch();
  const int stepped = gs->getFeedSelect();
  ASSERT_EQ(stepped, idx - 1);
  toMode(gs, FM_THREAD_REVERSE);
  EXPECT_EQ(gs->getFeedSelect(), stepped)
      << "next/prevFeedPitch() must write the shared slot too, not just "
         "setFeedSelect() called directly";
}

// ---------------------------------------------------------------------------
// A unit change while sitting in THREAD_REVERSE, with no intervening mode
// change. setUnitMode() must swap to the OTHER unit's thread slot immediately
// and must not disturb the feed mode.
// ---------------------------------------------------------------------------
TEST(PitchMemory, UnitChangeWhileInThreadReverseSwapsThreadSlotsAndKeepsMode) {
  GlobalState* gs = GlobalState::getInstance();

  const int metricThreadIdx = arraySizeFor(FM_THREAD, METRIC) - 2;
  const int imperialThreadIdx = arraySizeFor(FM_THREAD, IMPERIAL) - 3;
  ASSERT_GE(metricThreadIdx, 0);
  ASSERT_GE(imperialThreadIdx, 0);

  toState(gs, IMPERIAL, FM_THREAD);
  gs->setFeedSelect(imperialThreadIdx);
  toState(gs, METRIC, FM_THREAD);
  gs->setFeedSelect(metricThreadIdx);

  toMode(gs, FM_THREAD_REVERSE);
  ASSERT_EQ(gs->getFeedSelect(), metricThreadIdx);

  gs->setUnitMode(IMPERIAL);
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD_REVERSE)
      << "setUnitMode() must not change the feed mode";
  EXPECT_EQ(gs->getFeedSelect(), imperialThreadIdx)
      << "a unit change in THREAD_REVERSE must restore the new unit's THREAD "
         "slot straight away, without needing a mode change first";
  EXPECT_LT(gs->getCurrentFeedPitch(), 0.0f)
      << "still reverse, so the pitch must still be negative after the unit "
         "change";

  gs->setUnitMode(METRIC);
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD_REVERSE);
  EXPECT_EQ(gs->getFeedSelect(), metricThreadIdx)
      << "and back again";
}

// ---------------------------------------------------------------------------
// A mode change IMMEDIATELY after a unit change - the two restores happen
// back to back with no set in between, which is exactly the sequence the
// mode-cycle button produces when held (unit toggle) then clicked (mode step).
// ---------------------------------------------------------------------------
TEST(PitchMemory, ModeChangeImmediatelyAfterUnitChangeUsesTheNewUnitsSlot) {
  GlobalState* gs = GlobalState::getInstance();

  const int metricFeedIdx = arraySizeFor(FM_FEED, METRIC) - 1;
  const int imperialFeedIdx = arraySizeFor(FM_FEED, IMPERIAL) - 2;
  const int imperialThreadIdx = arraySizeFor(FM_THREAD, IMPERIAL) - 3;
  ASSERT_GE(metricFeedIdx, 0);
  ASSERT_GE(imperialFeedIdx, 0);
  ASSERT_GE(imperialThreadIdx, 0);

  // Seed all three slots involved.
  toState(gs, IMPERIAL, FM_FEED);
  gs->setFeedSelect(imperialFeedIdx);
  toMode(gs, FM_THREAD);
  gs->setFeedSelect(imperialThreadIdx);
  toState(gs, METRIC, FM_FEED);
  gs->setFeedSelect(metricFeedIdx);
  ASSERT_EQ(gs->getFeedSelect(), metricFeedIdx);

  // Unit toggle, then mode step, with nothing in between.
  gs->setUnitMode(IMPERIAL);
  EXPECT_EQ(gs->getFeedSelect(), imperialFeedIdx)
      << "unit change alone must land on IMPERIAL/FEED's remembered index";
  gs->IncFeedMode();
  ASSERT_EQ(gs->getFeedMode(), FM_THREAD);
  EXPECT_EQ(gs->getFeedSelect(), imperialThreadIdx)
      << "the mode step must use the NEW unit's thread slot, not the one that "
         "was current before the unit change";

  // And the metric feed slot must have survived the whole sequence untouched.
  toState(gs, METRIC, FM_FEED);
  EXPECT_EQ(gs->getFeedSelect(), metricFeedIdx);
}

// ---------------------------------------------------------------------------
// Item 3: setUnitMode() must not leave a stale/out-of-range index.
//
// The index is driven to the TOP of the current unit's array before every
// switch, deliberately: that is the only value that can be out of range for
// the other unit's array, and without it this test passes whatever
// setUnitMode() does. (Confirmed by mutation - with the index left to whatever
// the previous test happened to leave behind, reverting setUnitMode() to the
// old bare `m_unitMode = mode;` and shortening threadPitchImperial to 10
// entries still passed. It does not any more.)
// ---------------------------------------------------------------------------
TEST(PitchMemory, SetUnitModeLeavesFeedSelectInBoundsWithSanePitch) {
  GlobalState* gs = GlobalState::getInstance();

  for (GlobalFeedMode mode : {FM_FEED, FM_THREAD, FM_THREAD_REVERSE}) {
    for (GlobalUnitMode from : {METRIC, IMPERIAL}) {
      const GlobalUnitMode to = (from == METRIC) ? IMPERIAL : METRIC;
      toState(gs, from, mode);

      // Hostile starting point: the last valid index of the OLD array.
      const int topOfOld = arraySizeFor(mode, from) - 1;
      gs->setFeedSelect(topOfOld);
      ASSERT_EQ(gs->getFeedSelect(), topOfOld)
          << "test setup: could not park on the top index (mode=" << mode
          << ", unit=" << from << ")";

      gs->setUnitMode(to);

      const int size = arraySizeFor(mode, to);
      const int idx = gs->getFeedSelect();
      EXPECT_GE(idx, 0) << "feed select must not be negative after a unit "
                           "change (mode="
                        << mode << ", " << from << " -> " << to << ")";
      EXPECT_LT(idx, size) << "feed select must be in range for the "
                              "newly-selected array (mode="
                           << mode << ", " << from << " -> " << to << ")";

      float pitch = gs->getCurrentFeedPitch();
      EXPECT_NE(pitch, 0.0f) << "pitch must be non-zero after a unit change "
                                "(mode="
                             << mode << ", " << from << " -> " << to << ")";
      // Reverse must stay reverse across a unit change.
      if (mode == FM_THREAD_REVERSE) {
        EXPECT_LT(pitch, 0.0f);
      } else {
        EXPECT_GT(pitch, 0.0f);
      }
    }
  }
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
