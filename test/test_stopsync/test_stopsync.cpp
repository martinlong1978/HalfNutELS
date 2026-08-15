// Unit tests for LeadscrewStopSync (lib/leadscrew/leadscrew_stopsync.h).
//
// The struct is header-only state plus two cold mutators (setStop / unsetStop)
// defined in lib/leadscrew/leadscrew.cpp; the LDF pulls that library in via the
// include below, so these tests exercise the real production definitions
// directly on the plain struct (public fields), no Leadscrew rig required.
//
// Covers:
//  * characterization of setStop's "anchor only when at current position and
//    not already synced" contract and unsetStop's clearing behaviour, and
//  * the unsetStop re-anchor bug: when the stop holding the spindle-sync
//    anchor is unset while the OTHER stop is set, the anchor must MOVE to the
//    other stop with the spindle phase extrapolated across the gap:
//      newSync = positiveModulo((int)((dest - source) / ratio) + oldSync, PPR)

#include <gtest/gtest.h>

#include "config.h"
#include "leadscrew_stopsync.h"

namespace {

constexpr int kEncoderPPR = 1200;
// Non-trivial but exactly-representable ratio (5/8) so the expected values are
// exact in float arithmetic and a wrong formula (multiply instead of divide,
// wrong delta sign, enum-ordinal base) cannot coincidentally match.
constexpr float kRatio = 0.625f;

// The sync/helix relation used by Leadscrew::update():
//   spindlePhase(L) = positiveModulo((int)((L - syncStop)/ratio) + syncPos, PPR)
// Moving the anchor from `source` to `dest` must preserve that relation:
int expectedReanchor(int destStop, int sourceStop, int oldSyncPos, float ratio,
                     int ppr) {
  return positiveModulo(
      static_cast<int>(static_cast<float>(destStop - sourceStop) / ratio) +
          oldSyncPos,
      ppr);
}

// ---------------------------------------------------------------------------
// Characterization: current (correct) behaviour, guarded against regression.
// ---------------------------------------------------------------------------

TEST(StopSyncCharacterization, DefaultConstructedStateIsFullyUnset) {
  LeadscrewStopSync s;
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.leftStopPosition, INT32_MIN);
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.rightStopPosition, INT32_MAX);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::UNSET);
  EXPECT_EQ(s.spindleSyncPosition, 0);
}

TEST(StopSyncCharacterization, SetStopAtCurrentPositionEstablishesAnchor) {
  LeadscrewStopSync s;
  // Stop placed exactly at the current leadscrew position, no anchor yet.
  EXPECT_TRUE(s.setStop(LeadscrewStopPosition::LEFT, 100, 100, 37));
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::SET);
  EXPECT_EQ(s.leftStopPosition, 100);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::LEFT);
  EXPECT_EQ(s.spindleSyncPosition, 37);
}

TEST(StopSyncCharacterization, SetStopAwayFromCurrentPositionDoesNotAnchor) {
  LeadscrewStopSync s;
  // Stop position != current leadscrew position: recorded but no sync.
  EXPECT_FALSE(s.setStop(LeadscrewStopPosition::RIGHT, 1000, 100, 37));
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::SET);
  EXPECT_EQ(s.rightStopPosition, 1000);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::UNSET);
  EXPECT_EQ(s.spindleSyncPosition, 0);
}

TEST(StopSyncCharacterization, SetStopWhenAlreadySyncedKeepsExistingAnchor) {
  LeadscrewStopSync s;
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::LEFT, 100, 100, 37));
  // Second stop at the current position must NOT steal the anchor.
  EXPECT_FALSE(s.setStop(LeadscrewStopPosition::RIGHT, 100, 100, 999));
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::SET);
  EXPECT_EQ(s.rightStopPosition, 100);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::LEFT);
  EXPECT_EQ(s.spindleSyncPosition, 37);
}

TEST(StopSyncCharacterization, UnsetStopClearsStateAndRestoresSentinel) {
  LeadscrewStopSync s;
  s.setStop(LeadscrewStopPosition::LEFT, 100, 999, 0);   // no sync (999 != 100)
  s.setStop(LeadscrewStopPosition::RIGHT, 500, 999, 0);  // no sync
  s.unsetStop(LeadscrewStopPosition::LEFT, kRatio, kEncoderPPR);
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.leftStopPosition, INT32_MIN);
  s.unsetStop(LeadscrewStopPosition::RIGHT, kRatio, kEncoderPPR);
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.rightStopPosition, INT32_MAX);
}

TEST(StopSyncCharacterization, UnsetAnchorStopWithoutOtherStopDropsSync) {
  LeadscrewStopSync s;
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::LEFT, 100, 100, 37));
  // Right stop is NOT set; unsetting the anchoring left stop drops sync.
  s.unsetStop(LeadscrewStopPosition::LEFT, kRatio, kEncoderPPR);
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.leftStopPosition, INT32_MIN);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::UNSET);
}

TEST(StopSyncCharacterization, UnsetNonAnchorStopLeavesAnchorUntouched) {
  LeadscrewStopSync s;
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::LEFT, 100, 100, 37));
  s.setStop(LeadscrewStopPosition::RIGHT, 1000, 100, 0);
  // The RIGHT stop does not hold the anchor; unsetting it must not disturb it.
  s.unsetStop(LeadscrewStopPosition::RIGHT, kRatio, kEncoderPPR);
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.rightStopPosition, INT32_MAX);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::LEFT);
  EXPECT_EQ(s.spindleSyncPosition, 37);
}

// ---------------------------------------------------------------------------
// The bug: unsetStop must re-anchor onto the remaining stop by extrapolating
// the spindle phase across the gap between the stops.
// ---------------------------------------------------------------------------

TEST(StopSyncReanchor, UnsetAnchoringRightWithoutLeftStopDropsSync) {
  LeadscrewStopSync s;
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::RIGHT, 500, 500, 250));
  // Left stop is NOT set; unsetting the anchoring right stop must drop sync
  // (it must NOT re-anchor onto a nonexistent left stop).
  s.unsetStop(LeadscrewStopPosition::RIGHT, kRatio, kEncoderPPR);
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.rightStopPosition, INT32_MAX);
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::UNSET);
}

TEST(StopSyncReanchor, UnsetLeftMovesAnchorToRightWithExtrapolatedPhase) {
  LeadscrewStopSync s;
  const int leftStop = 100;
  const int rightStop = 1000;
  const int syncPhase = 37;

  // Anchor at LEFT with a known spindle phase, then set RIGHT elsewhere.
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::LEFT, leftStop, leftStop, syncPhase));
  ASSERT_FALSE(s.setStop(LeadscrewStopPosition::RIGHT, rightStop, leftStop, 555));
  ASSERT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::LEFT);

  s.unsetStop(LeadscrewStopPosition::LEFT, kRatio, kEncoderPPR);

  // Left stop fully cleared.
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.leftStopPosition, INT32_MIN);
  // Anchor moved to the RIGHT stop with the phase extrapolated across the gap.
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::RIGHT);
  // (1000 - 100) / 0.625 = 1440 exactly; +37 -> 1477 mod 1200 = 277.
  const int expected =
      expectedReanchor(rightStop, leftStop, syncPhase, kRatio, kEncoderPPR);
  ASSERT_EQ(expected, 277);
  EXPECT_EQ(s.spindleSyncPosition, expected);
  // Right stop untouched.
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::SET);
  EXPECT_EQ(s.rightStopPosition, rightStop);
}

TEST(StopSyncReanchor, UnsetRightMovesAnchorToLeftWithExtrapolatedPhase) {
  LeadscrewStopSync s;
  const int rightStop = 500;
  const int leftStop = -700;
  const int syncPhase = 250;

  // Anchor at RIGHT with a known spindle phase, then set LEFT elsewhere.
  ASSERT_TRUE(s.setStop(LeadscrewStopPosition::RIGHT, rightStop, rightStop, syncPhase));
  ASSERT_FALSE(s.setStop(LeadscrewStopPosition::LEFT, leftStop, rightStop, 999));
  ASSERT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::RIGHT);

  s.unsetStop(LeadscrewStopPosition::RIGHT, kRatio, kEncoderPPR);

  // Right stop fully cleared.
  EXPECT_EQ(s.rightStopState, LeadscrewStopState::UNSET);
  EXPECT_EQ(s.rightStopPosition, INT32_MAX);
  // Anchor moved to the LEFT stop with the phase extrapolated across the gap.
  EXPECT_EQ(s.syncPositionState, LeadscrewSpindleSyncPositionState::LEFT);
  // (-700 - 500) / 0.625 = -1920 exactly; +250 -> -1670 mod 1200 = 730.
  const int expected =
      expectedReanchor(leftStop, rightStop, syncPhase, kRatio, kEncoderPPR);
  ASSERT_EQ(expected, 730);
  EXPECT_EQ(s.spindleSyncPosition, expected);
  // Left stop untouched.
  EXPECT_EQ(s.leftStopState, LeadscrewStopState::SET);
  EXPECT_EQ(s.leftStopPosition, leftStop);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
