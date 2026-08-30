// Host tests for the OTA outcome policy (lib/ota/otaoutcome.h).
//
// This suite IS the specification for "an update can no longer be walked past".
// Everything the feature promises is pinned here:
//
//   * a failure and a success are never the same event - different headline,
//     different hold, and only one of them ever reboots;
//   * no failure path can reach OtaExit::RebootNow, from any state, ever
//     (the old code rebooted on every one of them, which is what made a failure
//     look like a success from across the shop);
//   * the detail line names the operator's next move, and a stall carries the
//     percentage, because a repeatable 18 % is a diagnosis and "Update failed"
//     is not;
//   * the modal holds far longer than the old 3 s but is BOUNDED, and the
//     evidence outlives it as a banner;
//   * a success confirms itself after the reboot, through OtaNotice.
//
// Pure host tests: no Arduino, no clock. Time is a parameter, exactly as in
// test/test_alarm.

#include <gmock/gmock.h>

#include <string.h>

#include <ostream>
#include <string>
#include <type_traits>

#include "otaoutcome.h"

std::ostream& operator<<(std::ostream& os, OtaResult r) {
  return os << "OtaResult::" << OtaOutcome::resultName(r);
}

std::ostream& operator<<(std::ostream& os, OtaExit e) {
  switch (e) {
    case OtaExit::Waiting: return os << "OtaExit::Waiting";
    case OtaExit::RebootNow: return os << "OtaExit::RebootNow";
    case OtaExit::ReturnToMachine: return os << "OtaExit::ReturnToMachine";
  }
  return os << "OtaExit::<?>";
}

std::ostream& operator<<(std::ostream& os, OtaPhase p) {
  return os << "OtaPhase(" << (int)p << ")";
}

namespace {

// Copies, so gtest never odr-uses the in-class constants by reference.
const unsigned long kSuccessHold = OtaOutcome::kSuccessHoldMs;
const unsigned long kInfoHold = OtaOutcome::kInfoHoldMs;
const unsigned long kFailHold = OtaOutcome::kFailureHoldMs;
const unsigned long kAckTimeout = OtaOutcome::kAckTimeoutMs;
const unsigned long kNotice = OtaOutcome::kNoticeMs;
const unsigned long kStall = OtaOutcome::kStallTimeoutMs;

const unsigned long kT0 = 100000;  // an arbitrary "boot was a while ago"

// A realistic image size, so the percentages in the messages are the ones the
// machine actually prints: 1.5 MB, the measured size of elstft.bin.
const unsigned long kImage = 1572864;

// Drives an attempt to the point of downloading, which is where most of the
// interesting failures live.
OtaOutcome* downloading(OtaOutcome& o, unsigned long now) {
  o.begin(now);
  o.notePhase(OtaPhase::Checking, now);
  o.noteVersion("1.4.2");
  o.notePhase(OtaPhase::Downloading, now);
  return &o;
}

std::string detailOf(const OtaOutcome& o) { return std::string(o.detail()); }

// --- The resting state -----------------------------------------------------

TEST(OtaOutcome, StartsUnsettledAndAsksForNothing) {
  OtaOutcome o;
  EXPECT_EQ(o.result(), OtaResult::InProgress);
  EXPECT_EQ(o.phase(), OtaPhase::Idle);
  EXPECT_FALSE(o.settled());
  EXPECT_FALSE(o.failed());
  EXPECT_FALSE(o.acknowledged());
  EXPECT_EQ(o.exitAction(kT0), OtaExit::Waiting);
  EXPECT_FALSE(o.noticePending(kT0));
  EXPECT_EQ(o.percent(), -1);
  // Constructors must initialise ALL members (CLAUDE.md): the strings are
  // readable before anything has happened, not garbage from the heap.
  EXPECT_STRNE(o.headline(), "");
  EXPECT_EQ(std::string(o.version()), "");
}

TEST(OtaOutcome, AnAttemptInFlightNeverExits) {
  OtaOutcome o;
  o.begin(kT0);
  for (unsigned long t = 0; t < 10 * kFailHold; t += 1000) {
    ASSERT_EQ(o.exitAction(kT0 + t), OtaExit::Waiting) << "at t=" << t;
  }
}

// --- Success ---------------------------------------------------------------

TEST(OtaOutcome, SuccessHoldsBrieflyThenReboots) {
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(kImage, kImage, kT0 + 13000);
  o.notePhase(OtaPhase::Finishing, kT0 + 13000);
  o.succeed(kT0 + 14000);

  EXPECT_EQ(o.result(), OtaResult::Success);
  EXPECT_TRUE(o.settled());
  EXPECT_FALSE(o.failed());
  EXPECT_FALSE(o.requiresAck());
  EXPECT_EQ(o.holdMs(), kSuccessHold);

  EXPECT_EQ(o.exitAction(kT0 + 14000), OtaExit::Waiting);
  EXPECT_EQ(o.exitAction(kT0 + 14000 + kSuccessHold - 1), OtaExit::Waiting);
  EXPECT_EQ(o.exitAction(kT0 + 14000 + kSuccessHold), OtaExit::RebootNow);
}

TEST(OtaOutcome, SuccessSaysWhichVersionIsNowRunning) {
  OtaOutcome o;
  downloading(o, kT0);
  o.succeed(kT0 + 1);
  EXPECT_EQ(std::string(o.headline()), "UPDATED");
  EXPECT_THAT(detailOf(o), testing::HasSubstr("1.4.2"));
}

TEST(OtaOutcome, SuccessWithAnUncheckedVersionSaysSoInsteadOfInventingOne) {
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Checking, kT0);
  o.noteVersionUnknown();  // GitHub API did not answer; we downloaded anyway
  o.notePhase(OtaPhase::Downloading, kT0);
  o.succeed(kT0 + 1);
  EXPECT_EQ(std::string(o.headline()), "UPDATED");
  EXPECT_FALSE(o.versionKnown());
  EXPECT_EQ(detailOf(o), "New image installed");
}

TEST(OtaOutcome, AcknowledgingASuccessRebootsImmediately) {
  // The hold exists to stop the outcome being missed. A keypress proves it was
  // not, so there is nothing left to wait for.
  OtaOutcome o;
  downloading(o, kT0);
  o.succeed(kT0);
  o.acknowledge();
  EXPECT_EQ(o.exitAction(kT0), OtaExit::RebootNow);
}

// --- Already up to date ----------------------------------------------------

TEST(OtaOutcome, UpToDateIsItsOwnOutcomeAndDoesNotReboot) {
  // The old code showed "No update available" for 3 s and then rebooted, which
  // is indistinguishable from an update. Nothing was written, so there is
  // nothing to reboot into: go back to the machine screen.
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Checking, kT0);
  o.noteVersion("1.4.2");
  o.upToDate(kT0 + 500);

  EXPECT_EQ(o.result(), OtaResult::UpToDate);
  EXPECT_FALSE(o.failed());
  EXPECT_FALSE(o.requiresAck());
  EXPECT_EQ(o.holdMs(), kInfoHold);
  EXPECT_EQ(std::string(o.headline()), "UP TO DATE");
  EXPECT_THAT(detailOf(o), testing::HasSubstr("1.4.2"));

  EXPECT_EQ(o.exitAction(kT0 + 500 + kInfoHold - 1), OtaExit::Waiting);
  EXPECT_EQ(o.exitAction(kT0 + 500 + kInfoHold), OtaExit::ReturnToMachine);
  // ...and it stays that way. No reboot, ever, on this path.
  EXPECT_EQ(o.exitAction(kT0 + 500 + 10 * kAckTimeout), OtaExit::ReturnToMachine);
}

TEST(OtaOutcome, UpToDateHoldsLongerThanTheOldThreeSeconds) {
  EXPECT_GT(kInfoHold, 3000u);
}

// --- Failures: the headline ------------------------------------------------

TEST(OtaOutcome, EveryFailureSharesOneUnmistakableHeadline) {
  const OtaResult reasons[] = {OtaResult::NoNetwork, OtaResult::NoServer,
                               OtaResult::DownloadStalled, OtaResult::BadImage};
  for (OtaResult r : reasons) {
    OtaOutcome o;
    o.begin(kT0);
    o.fail(r, kT0 + 100);
    EXPECT_EQ(o.result(), r);
    EXPECT_TRUE(o.failed()) << r;
    EXPECT_EQ(std::string(o.headline()), "UPDATE FAILED") << r;
    // ...and it is not the success headline, which is the whole point.
    EXPECT_NE(std::string(o.headline()), "UPDATED") << r;
    EXPECT_FALSE(detailOf(o).empty()) << r;
  }
}

// --- Failures: the detail names the next move ------------------------------

TEST(OtaOutcome, NoNetworkPointsAtTheNetworkSettings) {
  OtaOutcome o;
  o.begin(kT0);
  o.fail(OtaResult::NoNetwork, kT0 + 20000);
  EXPECT_THAT(detailOf(o), testing::HasSubstr("Wi-Fi"));
}

TEST(OtaOutcome, NoServerCarriesTheHttpCodeWhenThereIsOne) {
  OtaOutcome o;
  o.begin(kT0);
  o.noteVersion("1.4.2");
  o.fail(OtaResult::NoServer, kT0 + 5000, 404);
  EXPECT_EQ(o.code(), 404);
  EXPECT_THAT(detailOf(o), testing::HasSubstr("404"));
}

TEST(OtaOutcome, NoServerWithoutACodeStillSaysSomethingUseful) {
  // The RSSI -86 dBm case: TLS connect itself fails, so there is no status.
  OtaOutcome o;
  o.begin(kT0);
  o.noteVersion("1.4.2");
  o.fail(OtaResult::NoServer, kT0 + 15000);
  EXPECT_EQ(detailOf(o), "Server unreachable");
}

TEST(OtaOutcome, AFailureWithAnUncheckedVersionSaysTheCheckNeverHappened) {
  // Because "no server" after a silent version-check failure is two faults
  // reported as one, and the operator cannot tell whether ANYTHING was checked.
  OtaOutcome o;
  o.begin(kT0);
  o.noteVersionUnknown();
  o.fail(OtaResult::NoServer, kT0 + 15000);
  EXPECT_THAT(detailOf(o), testing::HasSubstr("version unchecked"));
}

TEST(OtaOutcome, AStallReportsThePercentageBecauseThePercentageIsTheDiagnosis) {
  // The measured modem-sleep fault hangs at a repeatable ~18 %. That number is
  // the difference between "the link is bad" and "WiFi.setSleep(false) is
  // missing" - and it is invisible from the word "failed".
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(283115, kImage, kT0 + 4000);  // ~18 %
  o.failIfStalled(kT0 + 4000 + kStall);

  EXPECT_EQ(o.result(), OtaResult::DownloadStalled);
  EXPECT_EQ(o.percent(), 17);  // 283115/1572864 = 17.99 %, truncated
  EXPECT_THAT(detailOf(o), testing::HasSubstr("17%"));
  EXPECT_THAT(detailOf(o), testing::HasSubstr("1.5 MB"));
  EXPECT_EQ(o.bytesDone(), 283115u);
  EXPECT_EQ(o.bytesTotal(), kImage);
}

TEST(OtaOutcome, AStallWithNoContentLengthDegradesToPlainWords) {
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Downloading, kT0);
  o.fail(OtaResult::DownloadStalled, kT0 + kStall);
  EXPECT_EQ(o.percent(), -1);
  EXPECT_EQ(detailOf(o), "Download stalled");
}

TEST(OtaOutcome, BadImageCarriesTheUpdateLibrarysOwnWordsWhenGiven) {
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::BadImage, kT0 + 30000, 4, "Bad Size Given");
  EXPECT_THAT(detailOf(o), testing::HasSubstr("Bad Size Given"));
  // The note is COPIED: Update.errorString() points at library-owned storage.
  EXPECT_THAT(detailOf(o), testing::HasSubstr("Bad image"));
}

TEST(OtaOutcome, DetailNeverOverrunsItsBuffer) {
  OtaOutcome o;
  downloading(o, kT0);
  std::string huge(400, 'x');
  o.fail(OtaResult::BadImage, kT0, 1, huge.c_str());
  EXPECT_LT(detailOf(o).size(), (size_t)OtaOutcome::kDetailLen);
  EXPECT_LT(std::string(o.headline()).size(), (size_t)OtaOutcome::kHeadlineLen);
}

TEST(OtaOutcome, AnOverlongVersionTagIsTruncatedNotSplattered) {
  OtaOutcome o;
  o.begin(kT0);
  std::string tag(200, 'v');
  o.noteVersion(tag.c_str());
  o.succeed(kT0 + 1);
  EXPECT_LT(std::string(o.version()).size(), (size_t)OtaOutcome::kVersionLen);
  EXPECT_LT(detailOf(o).size(), (size_t)OtaOutcome::kDetailLen);
}

// --- Failures: the exit policy ---------------------------------------------

TEST(OtaOutcome, NoFailureEverReachesAReboot) {
  // THE test. Every failure, every instant from settling to well past every
  // timeout in the class: the answer is never RebootNow. A reboot on failure is
  // exactly what made a broken update look like a working one.
  //
  // 4 * kAckTimeoutMs is now 20 minutes of virtual time (kAckTimeoutMs having
  // grown from 120 s to 300 s) - still ~1228 steps per reason/acked
  // combination at 977 ms, i.e. under 10k exitAction() calls total. Cheap on
  // the host regardless; widen the step rather than shortening the walk if
  // that ever stops being true.
  const OtaResult reasons[] = {OtaResult::NoNetwork, OtaResult::NoServer,
                               OtaResult::DownloadStalled, OtaResult::BadImage};
  for (OtaResult r : reasons) {
    for (int acked = 0; acked < 2; ++acked) {
      OtaOutcome o;
      downloading(o, kT0);
      o.fail(r, kT0 + 1000);
      if (acked) {
        o.acknowledge();
      }
      for (unsigned long t = 0; t < 4 * kAckTimeout; t += 977) {
        ASSERT_NE(o.exitAction(kT0 + 1000 + t), OtaExit::RebootNow)
            << r << " acked=" << acked << " t=" << t;
      }
    }
  }
}

// --- The floor vs. the unattended dwell: two DIFFERENT numbers -------------
//
// Owner-settled (Aug 2026), after the ack key was wired and the question
// became "what should the UNATTENDED backstop be, now that an attended
// operator can release it with OK immediately":
//
//   kFailureHoldMs (10 s)  - the UN-DISMISSABLE FLOOR. Its only job is to stop
//                            a stray keypress clearing the notice before it
//                            has been readable at all. An operator standing
//                            right there, who has already read "UPDATE
//                            FAILED - <reason>", is not made to wait any
//                            longer than that for OK to do something.
//   kAckTimeoutMs (300 s)  - the UNATTENDED SELF-RELEASE. This is the number
//                            that matters when nobody is watching, and the
//                            owner's own words on it: "I don't want to miss
//                            it if I'm away in the workshop looking at
//                            something else." It is NOT a second, longer
//                            "please dismiss this" floor - see the note below.
//
// The two are easy to conflate because NOTHING ON SCREEN DISTINGUISHES THEM -
// the modal looks identical whether it is 3 s past the floor or 3 s from the
// timeout. Do not let a future edit narrow the gap by "rounding them together
// for simplicity"; they answer different questions.
//
// The cost of the 300 s dwell is real and DELIBERATE: while hasOTA() is true,
// timerCallback() (src/main.cpp) runs commsManager.loop() instead of
// spindle->update() + leadscrew->update(), so an unattended, unacknowledged
// failure leaves the motion loop idle for up to five minutes. That is judged
// acceptable because the operator just asked for the update themselves (the
// machine is idle for that reason already) and because the wired ack key
// means an attended operator is never actually waiting that long - but it is
// worth stating plainly here, because "the lathe was dead for five minutes"
// is exactly the kind of thing that reads as a hang to whoever meets it next
// without this context.
TEST(OtaOutcome, TheFloorAndTheUnattendedDwellAreDeliberatelyFarApart) {
  EXPECT_EQ(kFailHold, 10000u);
  EXPECT_EQ(kAckTimeout, 300000u);
  EXPECT_GT(kAckTimeout, kFailHold);
}

TEST(OtaOutcome, AFailureHoldsLongEnoughNotToBeDismissedInstantly) {
  // The floor (10 s) is about PREMATURE dismissal only - it says nothing
  // about how long the modal stays up when nobody presses anything, which is
  // kAckTimeoutMs's job (see TheFloorAndTheUnattendedDwellAreDeliberatelyFarApart).
  EXPECT_EQ(kFailHold, 10000u);
  EXPECT_GT(kFailHold, kSuccessHold);
  EXPECT_GT(kFailHold, kInfoHold);

  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::DownloadStalled, kT0 + 5000);
  EXPECT_EQ(o.holdMs(), kFailHold);
  EXPECT_TRUE(o.requiresAck());
  // Still on screen long after the old code had rebooted and forgotten (3 s).
  EXPECT_EQ(o.exitAction(kT0 + 5000 + 3000), OtaExit::Waiting);
  EXPECT_EQ(o.exitAction(kT0 + 5000 + kFailHold - 1), OtaExit::Waiting);
}

TEST(OtaOutcome, AnUnacknowledgedFailureKeepsAskingUntilTheAckTimeout) {
  // Five minutes of virtual time, 500 ms steps - 600 exitAction() calls, cheap
  // on the host regardless of how large kAckTimeoutMs is.
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::NoServer, kT0, 500);
  for (unsigned long t = 0; t < kAckTimeout; t += 500) {
    ASSERT_EQ(o.exitAction(kT0 + t), OtaExit::Waiting) << "t=" << t;
  }
  // ...and then releases the machine rather than parking it on a dialog that
  // nobody is coming to press.
  EXPECT_EQ(o.exitAction(kT0 + kAckTimeout), OtaExit::ReturnToMachine);
}

TEST(OtaOutcome, AcknowledgingAFailureReleasesItImmediately) {
  // Even though the unattended dwell is now five minutes, a keypress at 10 ms
  // still releases at once - the whole point of wiring the ack key is that an
  // attended operator never has to wait for kAckTimeoutMs at all.
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::NoNetwork, kT0);
  EXPECT_EQ(o.exitAction(kT0 + 10), OtaExit::Waiting);
  o.acknowledge();
  EXPECT_TRUE(o.acknowledged());
  EXPECT_EQ(o.exitAction(kT0 + 10), OtaExit::ReturnToMachine);
}

TEST(OtaOutcome, TheAckTimeoutIsLongerThanTheFailureHoldSoBothWindowsExist) {
  // If these were the other way round the hold would be unreachable and the
  // acknowledgement request would never actually be made. 300 s vs 10 s is a
  // 30x gap - comfortably ordered, not a near thing that a future rounding
  // could accidentally invert.
  EXPECT_GT(kAckTimeout, kFailHold);
}

// --- The banner: evidence that outlives the dialog -------------------------

TEST(OtaOutcome, AnUnacknowledgedFailureLeavesABannerForever) {
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::DownloadStalled, kT0);
  EXPECT_TRUE(o.noticePending(kT0));
  EXPECT_TRUE(o.noticePending(kT0 + kAckTimeout));
  // A whole shift later, the machine still says the update did not apply.
  EXPECT_TRUE(o.noticePending(kT0 + 8UL * 3600UL * 1000UL));
}

TEST(OtaOutcome, AcknowledgingOrClearingTakesTheBannerDown) {
  OtaOutcome a;
  downloading(a, kT0);
  a.fail(OtaResult::NoServer, kT0, 404);
  a.acknowledge();
  EXPECT_FALSE(a.noticePending(kT0));

  OtaOutcome b;
  downloading(b, kT0);
  b.fail(OtaResult::NoServer, kT0, 404);
  b.clearNotice();
  EXPECT_FALSE(b.noticePending(kT0));
}

TEST(OtaOutcome, SuccessAndUpToDateLeaveNoBannerInThisBoot) {
  // A success's banner belongs to the NEXT boot (see restore()); showing one
  // here would be claiming victory two seconds before the reboot proves it.
  OtaOutcome s;
  downloading(s, kT0);
  s.succeed(kT0);
  EXPECT_FALSE(s.noticePending(kT0));

  OtaOutcome u;
  u.begin(kT0);
  u.upToDate(kT0);
  EXPECT_FALSE(u.noticePending(kT0));
}

TEST(OtaOutcome, RetryingClearsTheOldFailureAndItsBanner) {
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::NoNetwork, kT0);
  ASSERT_TRUE(o.noticePending(kT0));

  o.begin(kT0 + 60000);  // the operator pressed update again
  EXPECT_FALSE(o.settled());
  EXPECT_FALSE(o.failed());
  EXPECT_FALSE(o.noticePending(kT0 + 60000));
  EXPECT_FALSE(o.acknowledged());
  EXPECT_EQ(o.bytesDone(), 0u);
  EXPECT_EQ(o.percent(), -1);
  EXPECT_EQ(o.exitAction(kT0 + 60000 + kFailHold), OtaExit::Waiting);
}

// --- Across the reboot -----------------------------------------------------

TEST(OtaNoticeStruct, IsTriviallyCopyableSoItCanBeBlittedIntoRtcMemory) {
  EXPECT_TRUE(std::is_trivially_copyable<OtaNotice>::value);
  EXPECT_TRUE(std::is_standard_layout<OtaNotice>::value);
}

TEST(OtaOutcome, GarbageIsNotANotice) {
  OtaNotice n;
  memset(&n, 0xA5, sizeof(n));
  EXPECT_FALSE(OtaOutcome::noticeValid(n));

  OtaNotice zero;
  memset(&zero, 0, sizeof(zero));
  EXPECT_FALSE(OtaOutcome::noticeValid(zero));

  // Right magic, impossible result.
  OtaNotice bad;
  memset(&bad, 0, sizeof(bad));
  bad.magic = OtaOutcome::kNoticeMagic;
  bad.result = 200;
  EXPECT_FALSE(OtaOutcome::noticeValid(bad));

  // Right magic, but "still running" is not a thing worth carrying across.
  OtaNotice inflight;
  memset(&inflight, 0, sizeof(inflight));
  inflight.magic = OtaOutcome::kNoticeMagic;
  inflight.result = (uint8_t)OtaResult::InProgress;
  EXPECT_FALSE(OtaOutcome::noticeValid(inflight));
}

TEST(OtaOutcome, RestoringGarbageChangesNothing) {
  OtaOutcome o;
  OtaNotice n;
  memset(&n, 0xFF, sizeof(n));
  o.restore(n, kT0);
  EXPECT_FALSE(o.settled());
  EXPECT_FALSE(o.noticePending(kT0));
}

TEST(OtaOutcome, ASuccessConfirmsItselfOnTheNewFirmwaresFirstScreen) {
  // This is the only unambiguous proof an update applied, because the banner is
  // drawn by code that only runs if the new image booted.
  OtaOutcome before;
  downloading(before, kT0);
  before.noteProgress(kImage, kImage, kT0 + 13000);
  before.succeed(kT0 + 14000);
  const OtaNotice n = before.snapshot();
  ASSERT_TRUE(OtaOutcome::noticeValid(n));

  OtaOutcome after;  // fresh object, "new firmware", clock back near zero
  after.restore(n, 3000);
  EXPECT_EQ(after.result(), OtaResult::Success);
  EXPECT_EQ(std::string(after.headline()), "UPDATED");
  EXPECT_THAT(std::string(after.detail()), testing::HasSubstr("1.4.2"));
  EXPECT_TRUE(after.noticePending(3000));
  // Seen once, then it gets out of the way.
  EXPECT_TRUE(after.noticePending(3000 + kNotice - 1));
  EXPECT_FALSE(after.noticePending(3000 + kNotice));
}

TEST(OtaOutcome, ARestoredSuccessNeverAsksForAnotherReboot) {
  // Otherwise the "it worked" banner is a boot loop.
  OtaOutcome before;
  downloading(before, kT0);
  before.succeed(kT0);
  OtaOutcome after;
  after.restore(before.snapshot(), 1000);
  for (unsigned long t = 0; t < 4 * kAckTimeout; t += 997) {
    ASSERT_NE(after.exitAction(1000 + t), OtaExit::RebootNow) << "t=" << t;
  }
  EXPECT_EQ(after.exitAction(1000 + kSuccessHold), OtaExit::ReturnToMachine);
}

TEST(OtaOutcome, ARestoredFailureStillCarriesTheNumbers) {
  // If the operator does reboot a failed device, the reason survives it.
  OtaOutcome before;
  downloading(before, kT0);
  before.noteProgress(283115, kImage, kT0 + 4000);
  before.failIfStalled(kT0 + 4000 + kStall);

  OtaOutcome after;
  after.restore(before.snapshot(), 500);
  EXPECT_EQ(after.result(), OtaResult::DownloadStalled);
  EXPECT_TRUE(after.failed());
  EXPECT_EQ(after.percent(), 17);
  EXPECT_THAT(std::string(after.detail()), testing::HasSubstr("17%"));
  EXPECT_TRUE(after.noticePending(500 + 10 * kNotice));
}

// --- The stall watchdog ----------------------------------------------------

TEST(OtaOutcome, AHealthyDownloadNeverLooksStalled) {
  OtaOutcome o;
  downloading(o, kT0);
  unsigned long done = 0;
  for (unsigned long t = 0; t < 40000; t += 100) {
    done += 4096;
    o.noteProgress(done, kImage, kT0 + t);
    ASSERT_FALSE(o.stalled(kT0 + t)) << "t=" << t;
    ASSERT_FALSE(o.failIfStalled(kT0 + t)) << "t=" << t;
  }
  EXPECT_FALSE(o.settled());
}

TEST(OtaOutcome, ASlowButLiveDownloadIsNotAStall) {
  // The modem-sleep fault delivered one 1364-byte MSS every ~350 ms. That is
  // wretched (and diagnosed elsewhere by throughput) but it is NOT a stall, and
  // killing it at 20 s would abort updates that would have finished.
  OtaOutcome o;
  downloading(o, kT0);
  unsigned long done = 0;
  for (unsigned long t = 0; t < 120000; t += 350) {
    done += 1364;
    o.noteProgress(done, kImage, kT0 + t);
    ASSERT_FALSE(o.stalled(kT0 + t)) << "t=" << t;
  }
}

TEST(OtaOutcome, ARepeatedProgressCallbackWithNoNewBytesIsNotLife) {
  // Update.onProgress can fire with an unchanged count; if that reset the
  // watchdog it would never fire at all.
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(283115, kImage, kT0);
  for (unsigned long t = 0; t < kStall; t += 500) {
    o.noteProgress(283115, kImage, kT0 + t);
  }
  EXPECT_TRUE(o.stalled(kT0 + kStall));
}

TEST(OtaOutcome, ADownloadThatNeverDeliversAByteStallsFromTheStart) {
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Downloading, kT0);
  EXPECT_FALSE(o.stalled(kT0 + kStall - 1));
  EXPECT_TRUE(o.stalled(kT0 + kStall));
}

TEST(OtaOutcome, TheWatchdogOnlyWatchesDownloads) {
  // A slow TLS handshake or a slow flash write is the HTTP layer's timeout to
  // police, not this one's - and Update.end() can legitimately take seconds.
  const OtaPhase quiet[] = {OtaPhase::Connecting, OtaPhase::Checking,
                            OtaPhase::Finishing};
  for (OtaPhase p : quiet) {
    OtaOutcome o;
    o.begin(kT0);
    o.notePhase(p, kT0);
    EXPECT_FALSE(o.stalled(kT0 + 10 * kStall)) << p;
    EXPECT_FALSE(o.failIfStalled(kT0 + 10 * kStall)) << p;
  }
}

TEST(OtaOutcome, TheStallTimeoutOutlastsTheHttpReadTimeout) {
  // HTTPClient::setTimeout(15000) should get first refusal, so that when it
  // works we report its (more specific) short read rather than a guess.
  EXPECT_GT(kStall, 15000u);
}

TEST(OtaOutcome, ASettledOutcomeStopsWatchingAndStopsMoving) {
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(1000, kImage, kT0);
  o.fail(OtaResult::NoServer, kT0 + 1000, 502);

  EXPECT_FALSE(o.stalled(kT0 + 10 * kStall));
  EXPECT_FALSE(o.failIfStalled(kT0 + 10 * kStall));
  // Late reports from an unwinding caller must not disturb the diagnosis.
  o.noteProgress(kImage, kImage, kT0 + 2000);
  o.notePhase(OtaPhase::Finishing, kT0 + 2000);
  o.succeed(kT0 + 2000);
  EXPECT_EQ(o.result(), OtaResult::NoServer);
  EXPECT_EQ(o.code(), 502);
  EXPECT_EQ(o.bytesDone(), 1000u);
  EXPECT_TRUE(o.failed());
}

TEST(OtaOutcome, TheFirstDiagnosisWins) {
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(283115, kImage, kT0);
  o.fail(OtaResult::DownloadStalled, kT0 + kStall);
  o.fail(OtaResult::BadImage, kT0 + kStall + 10, 4, "Bad Size Given");
  EXPECT_EQ(o.result(), OtaResult::DownloadStalled);
  EXPECT_THAT(detailOf(o), testing::HasSubstr("17%"));
}

TEST(OtaOutcome, FailCannotBeTalkedIntoReportingASuccess) {
  // Defence in depth: a caller that passes the wrong enum through the failure
  // path gets a failure, because the failure is the safe reading.
  OtaOutcome o;
  downloading(o, kT0);
  o.fail(OtaResult::Success, kT0);
  EXPECT_TRUE(o.failed());
  EXPECT_NE(o.result(), OtaResult::Success);
  EXPECT_EQ(std::string(o.headline()), "UPDATE FAILED");
  for (unsigned long t = 0; t < 4 * kAckTimeout; t += 1013) {
    ASSERT_NE(o.exitAction(kT0 + t), OtaExit::RebootNow);
  }
}

// --- Progress arithmetic ---------------------------------------------------

TEST(OtaOutcome, PercentIsIntegerAndClamped) {
  OtaOutcome o;
  downloading(o, kT0);
  EXPECT_EQ(o.percent(), -1);
  o.noteProgress(0, kImage, kT0);
  EXPECT_EQ(o.percent(), 0);
  o.noteProgress(kImage / 2, kImage, kT0 + 1000);
  EXPECT_EQ(o.percent(), 50);
  o.noteProgress(kImage, kImage, kT0 + 2000);
  EXPECT_EQ(o.percent(), 100);
  // A server that over-reports cannot produce 103 %.
  o.noteProgress(kImage + 99999, kImage, kT0 + 3000);
  EXPECT_EQ(o.percent(), 100);
}

TEST(OtaOutcome, ProgressNeverGoesBackwards) {
  OtaOutcome o;
  downloading(o, kT0);
  o.noteProgress(900000, kImage, kT0);
  o.noteProgress(1000, kImage, kT0 + 100);
  EXPECT_EQ(o.bytesDone(), 900000u);
}

// --- Clock ------------------------------------------------------------------

TEST(OtaOutcome, SurvivesTheMillisWraparound) {
  // millis() wraps every ~49 days and a lathe is left powered. Every comparison
  // in the class is an unsigned difference, so a hold that straddles the wrap
  // still expires exactly once.
  const unsigned long nearWrap = (unsigned long)0 - 5000UL;
  OtaOutcome o;
  o.begin(nearWrap);
  o.notePhase(OtaPhase::Downloading, nearWrap);
  o.noteProgress(1000, kImage, nearWrap);
  EXPECT_FALSE(o.stalled(nearWrap + kStall - 1));  // wraps past zero
  EXPECT_TRUE(o.stalled(nearWrap + kStall));

  o.fail(OtaResult::DownloadStalled, nearWrap + kStall);
  EXPECT_EQ(o.exitAction(nearWrap + kStall + kFailHold - 1), OtaExit::Waiting);
  o.acknowledge();
  EXPECT_EQ(o.exitAction(nearWrap + kStall + 1), OtaExit::ReturnToMachine);
}

// --- The contract with the display -----------------------------------------

TEST(OtaOutcome, EveryResultRendersSomethingDistinctToRead) {
  // A screen with a blank line on it is the old bug in a new place.
  const OtaResult all[] = {OtaResult::Success,   OtaResult::UpToDate,
                           OtaResult::NoNetwork, OtaResult::NoServer,
                           OtaResult::DownloadStalled, OtaResult::BadImage};
  for (OtaResult r : all) {
    OtaOutcome o;
    downloading(o, kT0);
    if (r == OtaResult::Success) {
      o.succeed(kT0);
    } else if (r == OtaResult::UpToDate) {
      o.upToDate(kT0);
    } else {
      o.fail(r, kT0);
    }
    EXPECT_FALSE(std::string(o.headline()).empty()) << r;
    EXPECT_FALSE(std::string(o.detail()).empty()) << r;
    EXPECT_STRNE(OtaOutcome::resultName(r), "?") << r;
    EXPECT_EQ(o.phase(), OtaPhase::Done) << r;
  }
}

TEST(OtaOutcome, TheInFlightPhasesEachSayWhereWeAre) {
  OtaOutcome o;
  o.begin(kT0);
  EXPECT_EQ(std::string(o.headline()), "CONNECTING");
  o.notePhase(OtaPhase::Checking, kT0);
  EXPECT_EQ(std::string(o.headline()), "CHECKING");
  o.notePhase(OtaPhase::Downloading, kT0);
  EXPECT_EQ(std::string(o.headline()), "UPDATING");
  o.notePhase(OtaPhase::Finishing, kT0);
  EXPECT_EQ(std::string(o.headline()), "UPDATING");
  EXPECT_FALSE(o.settled());
}

// --- The three scenarios end to end ----------------------------------------

TEST(OtaOutcome, ScenarioTheFilmingSessionFailureIsNowVisible) {
  // What actually happened: the update did not apply, the lathe rebooted into
  // the old firmware, and the fault was chased for hours. Replayed here.
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Checking, kT0 + 3000);
  o.noteVersion("1.4.2");
  o.notePhase(OtaPhase::Downloading, kT0 + 4000);
  o.noteProgress(283115, kImage, kT0 + 9000);  // the 18 % hang
  ASSERT_TRUE(o.failIfStalled(kT0 + 9000 + kStall));

  // 1. Not a success, and it does not look like one.
  EXPECT_TRUE(o.failed());
  EXPECT_EQ(std::string(o.headline()), "UPDATE FAILED");
  // 2. It says why, in words that name the next move.
  EXPECT_THAT(detailOf(o), testing::HasSubstr("Stalled at 17%"));
  // 3. It does not reboot into the old firmware pretending to be new.
  EXPECT_NE(o.exitAction(kT0 + 100000), OtaExit::RebootNow);
  // 4. It clears the 10 s floor (so OK is live) but, unattended, it keeps
  //    asking for the full five minutes - 25 s in is still well inside that
  //    window...
  EXPECT_EQ(o.exitAction(kT0 + 9000 + kStall + 25000), OtaExit::Waiting);
  // 5. ...and when nobody comes inside kAckTimeoutMs (5 min), the machine is
  //    still usable and still says so.
  const unsigned long later = kT0 + 9000 + kStall + kAckTimeout;
  EXPECT_EQ(o.exitAction(later), OtaExit::ReturnToMachine);
  EXPECT_TRUE(o.noticePending(later));
}

TEST(OtaOutcome, ScenarioAGoodUpdateIsStillQuick) {
  OtaOutcome o;
  o.begin(kT0);
  o.notePhase(OtaPhase::Checking, kT0 + 3000);
  o.noteVersion("1.4.3");
  o.notePhase(OtaPhase::Downloading, kT0 + 4000);
  for (unsigned long b = 0; b <= kImage; b += 65536) {
    o.noteProgress(b, kImage, kT0 + 4000 + b / 1024);
  }
  o.notePhase(OtaPhase::Finishing, kT0 + 18000);
  o.succeed(kT0 + 20000);

  EXPECT_EQ(o.exitAction(kT0 + 20000 + kSuccessHold), OtaExit::RebootNow);

  OtaOutcome rebooted;
  rebooted.restore(o.snapshot(), 2500);
  EXPECT_TRUE(rebooted.noticePending(2500));
  EXPECT_THAT(std::string(rebooted.detail()), testing::HasSubstr("1.4.3"));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
