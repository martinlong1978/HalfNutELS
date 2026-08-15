// Pins the feed-mode cycle and the reverse-thread pitch sign in GlobalState.
// Reverse thread reuses the thread pitch tables but returns a NEGATIVE pitch so
// that Leadscrew::setTargetPitchMM negates the ratio (opposite travel).
#include <gmock/gmock.h>

#include "config.h"
#include "globalstate.h"

namespace {

// The singleton persists across tests in a suite; cycle to a known FM_FEED.
GlobalState* toFeed(GlobalState* gs) {
  for (int i = 0; i < 6 && gs->getFeedMode() != FM_FEED; i++) {
    gs->IncFeedMode();
  }
  return gs;
}

}  // namespace

TEST(FeedMode, CycleIncludesReverseThread) {
  GlobalState* gs = GlobalState::getInstance();
  toFeed(gs);
  ASSERT_EQ(gs->getFeedMode(), FM_FEED);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_THREAD_REVERSE);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_JOG);
  gs->IncFeedMode();
  EXPECT_EQ(gs->getFeedMode(), FM_FEED);
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

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
