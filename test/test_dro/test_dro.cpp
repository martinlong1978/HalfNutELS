// Pins the DRO datum rules from docs/ux-redesign.md section 8, "The DRO datum".
//
// Two things are being locked down here:
//
//   * the resolution ORDER (first match wins), including the case the rules are
//     easiest to get wrong - exactly one stop set, where the user's Left/Right
//     preference must be IGNORED because it cannot be honoured;
//   * the SIGN convention. Position increases to the RIGHT (matching
//     LeadscrewDirection::RIGHT = 1), and relativePulses() is current - datum.
//     A left datum counts up rightward; a right datum counts negative leftward.
//     The sign assertions are written so that inverting the subtraction cannot
//     pass: every non-zero expectation names an exact signed value.
//
// Boundary of responsibility: elsewhere in the firmware an unset stop is
// carried as an INT32_MIN / INT32_MAX sentinel in the position field. That
// encoding is the CALLER's, not this module's - Dro takes explicit
// `leftStopSet` / `rightStopSet` / `manualZeroSet` booleans and must never
// inspect the pulse field of an entry whose flag is false. So there are
// deliberately no tests here that feed INT32_MIN/INT32_MAX as a POSITION;
// translating sentinels into the bool flags belongs to the code that builds a
// DroInput. Negative positions ARE tested - they are ordinary travel, since the
// power-on origin can sit to the right of a stop.
#include <gmock/gmock.h>

#include "dro.h"

namespace {

// Nothing set, no preference bias - tests opt in to what they need.
DroInput makeInput() {
  DroInput in;
  in.leftStopSet = false;
  in.leftStopPulses = 0;
  in.rightStopSet = false;
  in.rightStopPulses = 0;
  in.manualZeroSet = false;
  in.manualZeroPulses = 0;
  in.preference = DroDatumPreference::Left;
  return in;
}

// A typical setup: both stops set, left of right, origin to the left of both.
DroInput bothStops(DroDatumPreference pref) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 1000;
  in.rightStopSet = true;
  in.rightStopPulses = 5000;
  in.preference = pref;
  return in;
}

}  // namespace

// --------------------------------------------------------------------------
// 1. Resolution order - first match wins
// --------------------------------------------------------------------------

// Rule 1 outranks rule 2: a manual zero wins even with both stops set.
TEST(DroResolution, ManualZeroOutranksBothStops) {
  DroInput in = bothStops(DroDatumPreference::Left);
  in.manualZeroSet = true;
  in.manualZeroPulses = 3000;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::ManualZero);
  EXPECT_EQ(Dro::datumPulses(in), 3000);
}

// ...and the preference does not rescue a stop once a manual zero exists.
TEST(DroResolution, ManualZeroOutranksBothStopsWithRightPreference) {
  DroInput in = bothStops(DroDatumPreference::Right);
  in.manualZeroSet = true;
  in.manualZeroPulses = 3000;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::ManualZero);
  EXPECT_EQ(Dro::datumPulses(in), 3000);
}

// Rule 1 outranks rule 3 as well.
TEST(DroResolution, ManualZeroOutranksASingleStop) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 1000;
  in.manualZeroSet = true;
  in.manualZeroPulses = 2500;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::ManualZero);
  EXPECT_EQ(Dro::datumPulses(in), 2500);
}

// Rule 1 applies with no stops at all - it beats the origin fallback too.
TEST(DroResolution, ManualZeroWithNoStops) {
  DroInput in = makeInput();
  in.manualZeroSet = true;
  in.manualZeroPulses = -750;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::ManualZero);
  EXPECT_EQ(Dro::datumPulses(in), -750);
}

// Rule 2: both stops set, no manual zero -> the stop named by the preference.
TEST(DroResolution, BothStopsSetHonourPreferenceLeft) {
  DroInput in = bothStops(DroDatumPreference::Left);

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), 1000);
}

TEST(DroResolution, BothStopsSetHonourPreferenceRight) {
  DroInput in = bothStops(DroDatumPreference::Right);

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::RightStop);
  EXPECT_EQ(Dro::datumPulses(in), 5000);
}

// Rule 3, the one most likely to be got wrong: with exactly one stop set the
// preference CANNOT be honoured and must be ignored, not fallen back on.
// Preference says Left, but only the RIGHT stop exists.
TEST(DroResolution, OnlyRightStopSetIgnoresLeftPreference) {
  DroInput in = makeInput();
  in.rightStopSet = true;
  in.rightStopPulses = 5000;
  in.preference = DroDatumPreference::Left;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::RightStop);
  EXPECT_EQ(Dro::datumPulses(in), 5000);
}

// ...and the mirror: preference says Right, but only the LEFT stop exists.
TEST(DroResolution, OnlyLeftStopSetIgnoresRightPreference) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 1000;
  in.preference = DroDatumPreference::Right;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), 1000);
}

// The matching-preference single-stop cases resolve the same way.
TEST(DroResolution, OnlyLeftStopSetWithLeftPreference) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 1000;
  in.preference = DroDatumPreference::Left;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), 1000);
}

TEST(DroResolution, OnlyRightStopSetWithRightPreference) {
  DroInput in = makeInput();
  in.rightStopSet = true;
  in.rightStopPulses = 5000;
  in.preference = DroDatumPreference::Right;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::RightStop);
  EXPECT_EQ(Dro::datumPulses(in), 5000);
}

// Rule 4: nothing set -> power-on origin, datum 0, flagged REL on screen.
TEST(DroResolution, NothingSetFallsBackToOrigin) {
  DroInput in = makeInput();

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::Origin);
  EXPECT_EQ(Dro::datumPulses(in), 0);
}

// Origin is chosen regardless of the stored preference.
TEST(DroResolution, NothingSetFallsBackToOriginWithRightPreference) {
  DroInput in = makeInput();
  in.preference = DroDatumPreference::Right;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::Origin);
  EXPECT_EQ(Dro::datumPulses(in), 0);
}

// An unset entry's pulse field is garbage as far as this module is concerned:
// stale values left in the struct must not leak into the datum.
TEST(DroResolution, UnsetEntriesPulseFieldsAreIgnored) {
  DroInput in = makeInput();
  in.leftStopPulses = 111;    // stale, leftStopSet == false
  in.rightStopPulses = 222;   // stale, rightStopSet == false
  in.manualZeroPulses = 333;  // stale, manualZeroSet == false

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::Origin);
  EXPECT_EQ(Dro::datumPulses(in), 0);
  EXPECT_EQ(Dro::relativePulses(in, 750), 750);
}

// --------------------------------------------------------------------------
// 2/3. Sign convention - position increases to the RIGHT
// --------------------------------------------------------------------------

// Left datum reads 0 at the left stop and counts UP rightward.
TEST(DroSign, LeftDatumReadsZeroAtLeftStopAndPositiveToTheRight) {
  DroInput in = bothStops(DroDatumPreference::Left);  // left 1000, right 5000

  EXPECT_EQ(Dro::relativePulses(in, 1000), 0);
  EXPECT_EQ(Dro::relativePulses(in, 1600), 600);
  EXPECT_GT(Dro::relativePulses(in, 1600), 0);
  // At the far stop the reading is the full span.
  EXPECT_EQ(Dro::relativePulses(in, 5000), 4000);
}

// Right datum reads 0 at the right stop and counts NEGATIVE leftward.
TEST(DroSign, RightDatumReadsZeroAtRightStopAndNegativeToTheLeft) {
  DroInput in = bothStops(DroDatumPreference::Right);  // left 1000, right 5000

  EXPECT_EQ(Dro::relativePulses(in, 5000), 0);
  EXPECT_EQ(Dro::relativePulses(in, 4400), -600);
  EXPECT_LT(Dro::relativePulses(in, 4400), 0);
  // At the far (left) stop the reading is the negated span.
  EXPECT_EQ(Dro::relativePulses(in, 1000), -4000);
}

// Manual zero mid-travel: 0 at the zero point, negative left, positive right.
TEST(DroSign, ManualZeroMidTravelReadsBothWays) {
  DroInput in = bothStops(DroDatumPreference::Left);
  in.manualZeroSet = true;
  in.manualZeroPulses = 3000;

  EXPECT_EQ(Dro::relativePulses(in, 3000), 0);
  EXPECT_EQ(Dro::relativePulses(in, 2250), -750);
  EXPECT_LT(Dro::relativePulses(in, 2250), 0);
  EXPECT_EQ(Dro::relativePulses(in, 3750), 750);
  EXPECT_GT(Dro::relativePulses(in, 3750), 0);
}

// With no datum at all the reading is the raw position, sign included.
TEST(DroSign, OriginDatumReadsRawPosition) {
  DroInput in = makeInput();

  EXPECT_EQ(Dro::relativePulses(in, 0), 0);
  EXPECT_EQ(Dro::relativePulses(in, 1234), 1234);
  EXPECT_EQ(Dro::relativePulses(in, -1234), -1234);
}

// A single right stop still counts negative to its left - the preference being
// unhonourable does not flip the convention.
TEST(DroSign, SingleRightStopStillCountsNegativeLeftward) {
  DroInput in = makeInput();
  in.rightStopSet = true;
  in.rightStopPulses = 5000;
  in.preference = DroDatumPreference::Left;

  EXPECT_EQ(Dro::relativePulses(in, 5000), 0);
  EXPECT_EQ(Dro::relativePulses(in, 4000), -1000);
  EXPECT_EQ(Dro::relativePulses(in, 5500), 500);
}

// --------------------------------------------------------------------------
// 4. Edge cases
// --------------------------------------------------------------------------

// Datum and current coincide -> exactly 0, for every source.
TEST(DroEdge, DatumAndCurrentCoincideReadExactlyZero) {
  DroInput left = bothStops(DroDatumPreference::Left);
  EXPECT_EQ(Dro::relativePulses(left, left.leftStopPulses), 0);

  DroInput right = bothStops(DroDatumPreference::Right);
  EXPECT_EQ(Dro::relativePulses(right, right.rightStopPulses), 0);

  DroInput man = bothStops(DroDatumPreference::Left);
  man.manualZeroSet = true;
  man.manualZeroPulses = -420;
  EXPECT_EQ(Dro::relativePulses(man, -420), 0);

  DroInput origin = makeInput();
  EXPECT_EQ(Dro::relativePulses(origin, 0), 0);
}

// The power-on origin can sit to the RIGHT of a stop, so stop positions are
// legitimately negative. Nothing about the arithmetic changes.
TEST(DroEdge, NegativeStopPositionsWork) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = -3000;
  in.rightStopSet = true;
  in.rightStopPulses = -500;
  in.preference = DroDatumPreference::Left;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), -3000);
  EXPECT_EQ(Dro::relativePulses(in, -3000), 0);
  EXPECT_EQ(Dro::relativePulses(in, -500), 2500);   // rightward is positive
  EXPECT_EQ(Dro::relativePulses(in, -3200), -200);  // leftward is negative

  in.preference = DroDatumPreference::Right;
  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::RightStop);
  EXPECT_EQ(Dro::datumPulses(in), -500);
  EXPECT_EQ(Dro::relativePulses(in, -500), 0);
  EXPECT_EQ(Dro::relativePulses(in, -3000), -2500);
  EXPECT_EQ(Dro::relativePulses(in, 0), 500);
}

// Stops straddling the origin, with a negative manual zero between them.
TEST(DroEdge, DatumSpanningTheOrigin) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = -2000;
  in.rightStopSet = true;
  in.rightStopPulses = 2000;
  in.manualZeroSet = true;
  in.manualZeroPulses = -1000;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::ManualZero);
  EXPECT_EQ(Dro::datumPulses(in), -1000);
  EXPECT_EQ(Dro::relativePulses(in, -1000), 0);
  EXPECT_EQ(Dro::relativePulses(in, 0), 1000);
  EXPECT_EQ(Dro::relativePulses(in, -2000), -1000);
}

// A zero-valued datum is a real datum, not "unset": a stop parked exactly at
// the power-on origin must still resolve to that stop.
TEST(DroEdge, StopAtZeroIsStillADatum) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 0;
  in.preference = DroDatumPreference::Right;

  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), 0);
  EXPECT_EQ(Dro::relativePulses(in, 900), 900);
  EXPECT_EQ(Dro::relativePulses(in, -900), -900);
}

// Degenerate but reachable: both stops set to the same pulse position. The
// preference still selects a source, and both give the same datum.
TEST(DroEdge, CoincidentStopsStillResolveByPreference) {
  DroInput in = makeInput();
  in.leftStopSet = true;
  in.leftStopPulses = 1500;
  in.rightStopSet = true;
  in.rightStopPulses = 1500;

  in.preference = DroDatumPreference::Left;
  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::LeftStop);
  EXPECT_EQ(Dro::datumPulses(in), 1500);

  in.preference = DroDatumPreference::Right;
  EXPECT_EQ(Dro::resolveSource(in), DroDatumSource::RightStop);
  EXPECT_EQ(Dro::datumPulses(in), 1500);
}

// datumPulses() and relativePulses() must agree with resolveSource(): the
// reading is always current - datum for whichever source won.
TEST(DroEdge, RelativeIsCurrentMinusDatumForEverySource) {
  const int current = 4321;

  DroInput cases[4];
  cases[0] = bothStops(DroDatumPreference::Left);
  cases[1] = bothStops(DroDatumPreference::Right);
  cases[2] = bothStops(DroDatumPreference::Left);
  cases[2].manualZeroSet = true;
  cases[2].manualZeroPulses = -77;
  cases[3] = makeInput();

  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(Dro::relativePulses(cases[i], current),
              current - Dro::datumPulses(cases[i]))
        << "case " << i;
  }
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
