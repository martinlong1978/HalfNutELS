// Trivial smoke test: proves the native googletest harness builds and runs.
#include <gmock/gmock.h>

TEST(SmokeTest, HarnessRuns) { EXPECT_EQ(1, 1); }

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char **argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
