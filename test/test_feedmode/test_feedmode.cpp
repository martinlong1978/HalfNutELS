// Pins the feed-mode cycle and the reverse-thread pitch sign in GlobalState.
// Reverse thread reuses the thread pitch tables but returns a NEGATIVE pitch so
// that Leadscrew::setTargetPitchMM negates the ratio (opposite travel).
//
// This file also pins the ux-redesign (docs/ux-redesign.md) feed-mode changes:
//   - FM_JOG leaves the IncFeedMode() cycle entirely (Sec. 3 "Consequences").
//   - incJogSpeed()/decJogSpeed() clamp against ARRAY_SIZE(jogSpeeds) - 1 / 0,
//     not a hardcoded top index (Sec. 4 "JOG SPEED").
//   - next/prevFeedPitch() always move the pitch index and return the NEW
//     index, regardless of feed mode - the FM_JOG dispatch to inc/decJogSpeed
//     goes away with the mode (Sec. 4 "JOG SPEED").
//
// Per-(mode,unit) pitch memory (Sec. 4 "MODE") is covered in
// test/test_pitchmemory/test_pitchmemory.cpp.
#include <gmock/gmock.h>

#include "config.h"
#include "globalstate.h"

namespace {

// The singleton persists across tests in a suite; cycle to a known mode.
// Bounded to 8 steps so a broken cycle (e.g. one that never reaches `target`)
// fails the subsequent ASSERT_EQ instead of hanging.
GlobalState* toMode(GlobalState* gs, GlobalFeedMode target) {
  for (int i = 0; i < 8 && gs->getFeedMode() != target; i++) {
    gs->IncFeedMode();
  }
  return gs;
}

GlobalState* toFeed(GlobalState* gs) { return toMode(gs, FM_FEED); }

}  // namespace

// ---------------------------------------------------------------------------
// Item 1: FM_JOG leaves the feed-mode cycle.
// ---------------------------------------------------------------------------

// UPDATED per docs/ux-redesign.md Sec. 3: IncFeedMode() becomes a 3-way cycle
// FM_FEED -> FM_THREAD -> FM_THREAD_REVERSE -> FM_FEED. FM_JOG must never be
// produced by IncFeedMode() any more.
//
// This assertion is intentionally the CORRECT (new) behaviour, not the
// current one - per CLAUDE.md's testing convention it is expected to FAIL
// against today's code, which still cycles ...THREAD_REVERSE -> FM_JOG ->
// FM_FEED.
TEST(FeedMode, CycleIncludesReverseThread) {
  GlobalState* gs = GlobalState::getInstance();
  toFeed(gs);
  ASSERT_EQ(gs->getFeedMode(), FM_FEED);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD_REVERSE);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_FEED)
      << "IncFeedMode() must return straight to FM_FEED - FM_JOG is no "
         "longer part of the cycle";
}

// Belt-and-braces version of the above: cycle for a good long while and
// assert FM_JOG is never observed. Fails against current code on the 4th
// IncFeedMode() call (THREAD_REVERSE -> JOG).
TEST(FeedMode, CycleNeverReachesJog) {
  GlobalState* gs = GlobalState::getInstance();
  toFeed(gs);
  for (int i = 0; i < 20; i++) {
    gs->IncFeedMode();
    EXPECT_NE(gs->getFeedMode(), FM_JOG)
        << "IncFeedMode() produced FM_JOG on iteration " << i;
  }
}

TEST(FeedMode, ReverseThreadNegatesPitchSameMagnitude) {
  GlobalState* gs = GlobalState::getInstance();
  gs->setUnitMode(METRIC);
  toFeed(gs);

  gs->IncFeedMode();  // -> FM_THREAD
  ASSERT_EQ(gs->getFeedMode(), FM_THREAD);
  float threadPitch = gs->getCurrentFeedPitch();
  EXPECT_GT(threadPitch, 0.0f);

  gs->IncFeedMode();  // -> FM_THREAD_REVERSE
  ASSERT_EQ(gs->getFeedMode(), FM_THREAD_REVERSE);
  float reversePitch = gs->getCurrentFeedPitch();
  EXPECT_LT(reversePitch, 0.0f);
  // Same magnitude, opposite sign - the whole point of reverse thread.
  EXPECT_FLOAT_EQ(reversePitch, -threadPitch);
}

// ---------------------------------------------------------------------------
// Item 4: jog speed clamp derived from ARRAY_SIZE(jogSpeeds), not hardcoded.
// ---------------------------------------------------------------------------
//
// NOTE: with today's jogSpeeds[] (6 entries), ARRAY_SIZE(jogSpeeds) - 1 == 5,
// which is exactly the hardcoded top the current incJogSpeed() uses - so this
// test currently PASSES. It is still worth keeping: it pins the *contract*
// (derive the top from the array, don't hardcode it) so that adding a jog
// speed later doesn't silently make it unreachable, per docs/ux-redesign.md
// Sec. 4 ("adding a speed silently makes it unreachable").
TEST(FeedMode, JogSpeedClampsAtArrayBoundsDerivedFromArraySize) {
  GlobalState* gs = GlobalState::getInstance();
  const int topIndex = (int)ARRAY_SIZE(jogSpeeds) - 1;

  for (int i = 0; i < topIndex + 10; i++) gs->incJogSpeed();
  EXPECT_EQ(gs->getJogIndex(), topIndex);
  EXPECT_FLOAT_EQ(gs->getJogSpeed(), jogSpeeds[gs->getJogIndex()]);

  for (int i = 0; i < topIndex + 10; i++) gs->decJogSpeed();
  EXPECT_EQ(gs->getJogIndex(), 0);
  EXPECT_FLOAT_EQ(gs->getJogSpeed(), jogSpeeds[gs->getJogIndex()]);
}

// ---------------------------------------------------------------------------
// Item 5: next/prevFeedPitch() always move the pitch index; no jog dispatch;
// clamp at both ends without wrapping.
// ---------------------------------------------------------------------------

TEST(FeedMode, NextFeedPitchClampsAtTopWithoutWrapping) {
  GlobalState* gs = GlobalState::getInstance();
  gs->setUnitMode(METRIC);
  toFeed(gs);
  const int top = (int)ARRAY_SIZE(feedPitchMetric) - 1;
  gs->setFeedSelect(top);
  ASSERT_EQ(gs->getFeedSelect(), top);

  int idx = gs->nextFeedPitch();
  EXPECT_EQ(idx, top);
  EXPECT_EQ(gs->getFeedSelect(), top);
  // A second call must stay pinned at the top, not wrap to 0.
  idx = gs->nextFeedPitch();
  EXPECT_EQ(idx, top);
  EXPECT_EQ(gs->getFeedSelect(), top);
}

TEST(FeedMode, PrevFeedPitchClampsAtBottomWithoutWrapping) {
  GlobalState* gs = GlobalState::getInstance();
  gs->setUnitMode(METRIC);
  toFeed(gs);
  gs->setFeedSelect(0);
  ASSERT_EQ(gs->getFeedSelect(), 0);

  int idx = gs->prevFeedPitch();
  EXPECT_EQ(idx, 0);
  EXPECT_EQ(gs->getFeedSelect(), 0);
  // A second call must stay pinned at 0, not wrap to the top.
  idx = gs->prevFeedPitch();
  EXPECT_EQ(idx, 0);
  EXPECT_EQ(gs->getFeedSelect(), 0);
}

// FeedMode.NextPrevFeedPitchIgnoreJogSpeedEvenInJogMode used to live here. It
// reached FM_JOG by cycling IncFeedMode() around the old 4-way cycle and then
// asserted getFeedMode() == FM_JOG as a precondition. Once FM_JOG left the
// mode cycle (see CycleNeverReachesJog above), FM_JOG became unreachable via
// the public API - GlobalState has no setFeedMode() - so that precondition
// can never be satisfied again. Deleted rather than left as a permanently
// failing trap; do not re-add it. The FM_JOG dispatch it exercised inside
// next/prevFeedPitch() is gone too, for the same reason (dead code, provably
// unreachable). The still-meaningful half of the old test - that
// next/prevFeedPitch() return the NEW index and never touch jog speed -
// survives below, exercised over the modes that remain reachable.
TEST(FeedMode, NextPrevFeedPitchReturnNewIndexAndLeaveJogSpeedAlone) {
  GlobalState* gs = GlobalState::getInstance();
  gs->setUnitMode(METRIC);

  for (GlobalFeedMode mode : {FM_FEED, FM_THREAD}) {
    toMode(gs, mode);
    ASSERT_EQ(gs->getFeedMode(), mode);

    // Make sure the pitch index has room to move in both directions.
    gs->setFeedSelect(1);
    int pitchIdxBefore = gs->getFeedSelect();
    ASSERT_EQ(pitchIdxBefore, 1);
    int jogIdxBefore = gs->getJogIndex();

    int returned = gs->nextFeedPitch();
    EXPECT_EQ(gs->getJogIndex(), jogIdxBefore)
        << "nextFeedPitch() must not change jog speed";
    EXPECT_EQ(returned, pitchIdxBefore + 1)
        << "nextFeedPitch() must return the NEW pitch index";
    EXPECT_EQ(gs->getFeedSelect(), pitchIdxBefore + 1);

    int pitchIdxNow = gs->getFeedSelect();
    int jogIdxNow = gs->getJogIndex();
    int returned2 = gs->prevFeedPitch();
    EXPECT_EQ(gs->getJogIndex(), jogIdxNow)
        << "prevFeedPitch() must not change jog speed";
    EXPECT_EQ(returned2, pitchIdxNow - 1)
        << "prevFeedPitch() must return the NEW pitch index";
    EXPECT_EQ(gs->getFeedSelect(), pitchIdxNow - 1);
  }
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
