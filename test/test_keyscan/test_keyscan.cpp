// Debounce and gesture recognition for the keypad matrix (lib/keyscan).
//
// These tests are the specification. The behaviour they pin is the behaviour
// UiState is built on - Press/Click/Release for a tap, Press/Hold/Release for a
// long press, never a Click after a Hold - plus the two properties the old
// edge-interrupt scheme could not offer at all: a bounce burst can never lose a
// real transition, and no reading can leave the scanner unable to see the next
// one (docs/keypad-audit.md).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config.h"
#include "keyscan.h"

#include <vector>

namespace {

// Codes from lib/config/board.h. Any two distinct non-zero matrix codes would
// do; using the real ones keeps the tests readable next to the panel.
const int kOk = 18;
const int kMenu = 20;

// Drives the scanner the way src/keyarray.cpp does: one sample every `stepMs`,
// collecting whatever comes out.
class Rig {
 public:
  Rig() : m_now(0) {}

  // Feed `code` for `ms` milliseconds at the real 2 ms scan period.
  std::vector<KeyScanOut> feed(int code, unsigned long ms) {
    std::vector<KeyScanOut> all;
    const unsigned long end = m_now + ms;
    while (m_now < end) {
      KeyScanOut out[kKeyScanMaxEvents];
      const int n = m_scanner.update(code, m_now, out, kKeyScanMaxEvents);
      for (int i = 0; i < n; i++) {
        all.push_back(out[i]);
      }
      m_now += 2;
    }
    return all;
  }

  // One single sample, for the tests that care about an exact instant.
  std::vector<KeyScanOut> sample(int code) {
    std::vector<KeyScanOut> all;
    KeyScanOut out[kKeyScanMaxEvents];
    const int n = m_scanner.update(code, m_now, out, kKeyScanMaxEvents);
    for (int i = 0; i < n; i++) {
      all.push_back(out[i]);
    }
    return all;
  }

  void advance(unsigned long ms) { m_now += ms; }
  KeyScanner& scanner() { return m_scanner; }

 private:
  KeyScanner m_scanner;
  unsigned long m_now;
};

// The event sequence, ignoring which key, for readable comparisons.
std::vector<int> events(const std::vector<KeyScanOut>& v) {
  std::vector<int> out;
  for (size_t i = 0; i < v.size(); i++) {
    out.push_back(v[i].event);
  }
  return out;
}

}  // namespace

// ===========================================================================
// 1. The two gestures UiState is built on
// ===========================================================================

TEST(KeyScan, AShortTapIsPressClickRelease) {
  Rig r;
  std::vector<KeyScanOut> down = r.feed(kOk, 100);
  ASSERT_EQ(std::vector<int>({KS_PRESSED}), events(down))
      << "the press is reported as soon as it is stable, and only once";
  EXPECT_EQ(kOk, down[0].code);

  std::vector<KeyScanOut> up = r.feed(0, 100);
  EXPECT_EQ(std::vector<int>({KS_CLICKED, KS_RELEASED}), events(up))
      << "Click then Release, in that order";
  EXPECT_EQ(kOk, up[0].code);
  EXPECT_EQ(kOk, up[1].code) << "the release names the key that was released";
}

TEST(KeyScan, ALongPressIsPressHoldReleaseWithNoClick) {
  Rig r;
  std::vector<KeyScanOut> down = r.feed(kOk, kKeyHoldMs + 100);
  EXPECT_EQ(std::vector<int>({KS_PRESSED, KS_HELD}), events(down));

  std::vector<KeyScanOut> up = r.feed(0, 100);
  EXPECT_EQ(std::vector<int>({KS_RELEASED}), events(up))
      << "NO Click after a Hold - UiState relies on this to stop one gesture "
         "firing two actions";
}

TEST(KeyScan, TheHoldFiresOnceNoMatterHowLongItIsHeld) {
  Rig r;
  r.feed(kOk, kKeyHoldMs + 50);
  std::vector<KeyScanOut> more = r.feed(kOk, 5000);
  EXPECT_TRUE(more.empty()) << "a held key must not repeat";
}

TEST(KeyScan, TheHoldLandsAtTheThresholdNotBefore) {
  Rig r;
  // Just short of the threshold, measured from when the press became stable.
  std::vector<KeyScanOut> early = r.feed(kOk, kKeyHoldMs - 20);
  EXPECT_EQ(std::vector<int>({KS_PRESSED}), events(early));
  std::vector<KeyScanOut> late = r.feed(kOk, 40);
  EXPECT_EQ(std::vector<int>({KS_HELD}), events(late));
}

// ===========================================================================
// 2. Debouncing - what it must reject, and what it must NOT
// ===========================================================================

TEST(KeyScan, ChatterShorterThanTheDebounceProducesNothing) {
  Rig r;
  // A contact bouncing on and off every sample for 6 ms - shorter than the
  // debounce, so no reading ever becomes stable.
  std::vector<KeyScanOut> all;
  for (int i = 0; i < 3; i++) {
    std::vector<KeyScanOut> a = r.feed(kOk, 2);
    std::vector<KeyScanOut> b = r.feed(0, 2);
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
  }
  EXPECT_TRUE(all.empty()) << "bounce must not reach UiState as key events";
  EXPECT_EQ(0, r.scanner().stableCode());
  EXPECT_GT(r.scanner().bounceRejects(), 0u)
      << "and it must be COUNTED, so the threshold can be judged from the "
         "machine rather than guessed";
}

TEST(KeyScan, BounceOnTheWayInStillYieldsExactlyOnePress) {
  Rig r;
  // The realistic shape: a few milliseconds of chatter, then a solid contact.
  r.feed(kOk, 2);
  r.feed(0, 2);
  r.feed(kOk, 2);
  std::vector<KeyScanOut> settled = r.feed(kOk, 100);
  EXPECT_EQ(std::vector<int>({KS_PRESSED}), events(settled))
      << "one press, not three";
}

TEST(KeyScan, BounceOnTheWayOutStillYieldsExactlyOneRelease) {
  Rig r;
  r.feed(kOk, 100);  // clean press
  // Contact chattering as it opens.
  std::vector<KeyScanOut> all;
  for (int i = 0; i < 3; i++) {
    std::vector<KeyScanOut> a = r.feed(0, 2);
    std::vector<KeyScanOut> b = r.feed(kOk, 2);
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
  }
  std::vector<KeyScanOut> settled = r.feed(0, 100);
  all.insert(all.end(), settled.begin(), settled.end());
  EXPECT_EQ(std::vector<int>({KS_CLICKED, KS_RELEASED}), events(all))
      << "one Click and one Release, however the contact chattered";
}

// THE REGRESSION THE WHOLE REWRITE EXISTS FOR.
//
// Under the old edge-interrupt scheme a release arriving within 10 ms of its
// press was swallowed AND left the pad armed for an edge that could never come
// again - so the keypad went dead until the 1 s hold timer rescanned. Here a
// tap shorter than the debounce is simply not believed, and - the part that
// matters - the very next press is unaffected.
TEST(KeyScan, AVeryShortTapCannotLeaveTheScannerDeaf) {
  Rig r;
  r.feed(kOk, 4);  // shorter than kKeyDebounceMs: never becomes stable
  r.feed(0, 4);
  EXPECT_EQ(0, r.scanner().stableCode());

  // The next, deliberate press must work normally. THIS is what the old code
  // could not do.
  std::vector<KeyScanOut> down = r.feed(kOk, 100);
  EXPECT_EQ(std::vector<int>({KS_PRESSED}), events(down))
      << "a rejected tap must not cost the press that follows it";
  std::vector<KeyScanOut> up = r.feed(0, 100);
  EXPECT_EQ(std::vector<int>({KS_CLICKED, KS_RELEASED}), events(up));
}

TEST(KeyScan, ARealTapIsNeverRejectedHoweverBriskly) {
  // 50 ms is about as short as a deliberate human tap gets. Every one must
  // produce a complete gesture - no lost presses, no lost releases.
  for (unsigned long ms = 20; ms <= 120; ms += 10) {
    Rig r;
    std::vector<KeyScanOut> all = r.feed(kOk, ms);
    std::vector<KeyScanOut> up = r.feed(0, 100);
    all.insert(all.end(), up.begin(), up.end());
    EXPECT_EQ(std::vector<int>({KS_PRESSED, KS_CLICKED, KS_RELEASED}),
              events(all))
        << "tap of " << ms << " ms";
  }
}

// ===========================================================================
// 3. Rolling from one key to another
// ===========================================================================

TEST(KeyScan, RollingToAnotherKeyRetiresTheFirstAndOpensTheSecond) {
  Rig r;
  r.feed(kOk, 100);
  std::vector<KeyScanOut> roll = r.feed(kMenu, 100);
  ASSERT_EQ(std::vector<int>({KS_CLICKED, KS_RELEASED, KS_PRESSED}),
            events(roll))
      << "the first key must be closed out before the second opens, or UiState "
         "sees a Press with no matching Release";
  EXPECT_EQ(kOk, roll[0].code);
  EXPECT_EQ(kOk, roll[1].code);
  EXPECT_EQ(kMenu, roll[2].code);
}

TEST(KeyScan, RollingAfterAHoldStillSuppressesTheClick) {
  Rig r;
  r.feed(kOk, kKeyHoldMs + 50);
  std::vector<KeyScanOut> roll = r.feed(kMenu, 100);
  EXPECT_EQ(std::vector<int>({KS_RELEASED, KS_PRESSED}), events(roll));
  EXPECT_EQ(kOk, roll[0].code);
  EXPECT_EQ(kMenu, roll[1].code);
}

TEST(KeyScan, TheSecondKeyGetsItsOwnHoldWindow) {
  Rig r;
  r.feed(kOk, 100);
  r.feed(kMenu, 100);
  std::vector<KeyScanOut> later = r.feed(kMenu, kKeyHoldMs);
  EXPECT_EQ(std::vector<int>({KS_HELD}), events(later));
  EXPECT_EQ(kMenu, later[0].code);
}

// A two-key press reads as code 0 on this matrix (getCodeFromArray() cannot
// resolve it), so it must look like a release rather than a phantom key.
TEST(KeyScan, AnUnresolvableReadingClosesTheCurrentKey) {
  Rig r;
  r.feed(kOk, 100);
  std::vector<KeyScanOut> both = r.feed(0, 100);
  EXPECT_EQ(std::vector<int>({KS_CLICKED, KS_RELEASED}), events(both));
}

// ===========================================================================
// 4. Contract details the caller depends on
// ===========================================================================

TEST(KeyScan, StartsIdleAndStaysIdleWithNothingPressed) {
  Rig r;
  std::vector<KeyScanOut> quiet = r.feed(0, 5000);
  EXPECT_TRUE(quiet.empty());
  EXPECT_EQ(0, r.scanner().stableCode());
}

TEST(KeyScan, NeverExceedsTheDeclaredEventCount) {
  // The caller sizes its array from kKeyScanMaxEvents; update() must honour it.
  Rig r;
  r.feed(kOk, 100);
  KeyScanOut out[kKeyScanMaxEvents];
  for (int i = 0; i < 200; i++) {
    const int n = r.scanner().update(i % 2 ? kMenu : kOk, i * 2, out,
                                     kKeyScanMaxEvents);
    ASSERT_GE(n, 0);
    ASSERT_LE(n, kKeyScanMaxEvents);
  }
}

TEST(KeyScan, RespectsASmallerCallerBuffer) {
  Rig r;
  r.feed(kOk, 100);
  // Roll to another key, which wants to emit three, into room for one.
  KeyScanOut out[1];
  const int n = r.scanner().update(kMenu, 1000, out, 1);
  EXPECT_LE(n, 1) << "must never write past the caller's array";
}

TEST(KeyScan, StableCodeTracksWhatIsActuallyDown) {
  Rig r;
  EXPECT_EQ(0, r.scanner().stableCode());
  r.feed(kOk, 100);
  EXPECT_EQ(kOk, r.scanner().stableCode());
  r.feed(kMenu, 100);
  EXPECT_EQ(kMenu, r.scanner().stableCode());
  r.feed(0, 100);
  EXPECT_EQ(0, r.scanner().stableCode());
}

// millis() wraps every ~49 days. Every comparison in the scanner is an unsigned
// difference, so a press spanning the wrap behaves normally - it must not fire
// a Hold instantly, nor fail to fire one at all.
TEST(KeyScan, SurvivesAMillisWrapMidPress) {
  KeyScanner s;
  KeyScanOut out[kKeyScanMaxEvents];
  unsigned long t = (unsigned long)0xFFFFFF00u;  // 256 ms before the wrap

  int pressed = 0, held = 0;
  for (int i = 0; i < 800; i++) {  // 1600 ms across the wrap point
    const int n = s.update(kOk, t, out, kKeyScanMaxEvents);
    for (int j = 0; j < n; j++) {
      if (out[j].event == KS_PRESSED) pressed++;
      if (out[j].event == KS_HELD) held++;
    }
    t += 2;  // wraps through zero partway
  }
  EXPECT_EQ(1, pressed);
  EXPECT_EQ(1, held) << "the hold must fire exactly once across the wrap";
}

// PlatformIO does not inject a googletest runner for this env, so provide one.
int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
