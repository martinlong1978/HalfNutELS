#include "otaoutcome.h"

#include <stdio.h>
#include <string.h>

// Out-of-class definitions so gtest (and anything else) may take these by
// reference without an undefined symbol - same reason lib/alarm does it.
const unsigned long OtaOutcome::kSuccessHoldMs;
const unsigned long OtaOutcome::kInfoHoldMs;
const unsigned long OtaOutcome::kFailureHoldMs;
const unsigned long OtaOutcome::kAckTimeoutMs;
const unsigned long OtaOutcome::kNoticeMs;
const unsigned long OtaOutcome::kStallTimeoutMs;
const uint32_t OtaOutcome::kNoticeMagic;
const int OtaOutcome::kHeadlineLen;
const int OtaOutcome::kDetailLen;
const int OtaOutcome::kVersionLen;
const unsigned long OtaOutcome::kRateStaleMs;
const int OtaOutcome::kVersionTransitionLen;
const int OtaOutcome::kTransferLen;
const int OtaOutcome::kEtaLen;
const int OtaOutcome::kSignalLen;

namespace {

void copyInto(char* dst, int cap, const char* src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  int i = 0;
  for (; i < cap - 1 && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

// Tenths of a MiB, rounded, integer only. 1 572 864 -> 15 -> "1.5 MB". The
// half-divisor addition cannot overflow 32 bits for any byte count that fits in
// one, and floats have no business on the display task's path.
unsigned long tenthsOfMb(unsigned long bytes) {
  return (bytes + 52429UL) / 104858UL;
}

}  // namespace

OtaOutcome::OtaOutcome()
    : m_result(OtaResult::InProgress),
      m_phase(OtaPhase::Idle),
      m_code(0),
      m_bytesDone(0),
      m_bytesTotal(0),
      m_lastProgressMs(0),
      m_settledAtMs(0),
      m_downloading(false),
      m_versionKnown(false),
      m_noticeCleared(false),
      m_restored(false),
      m_acked(false),
      m_nowMs(0),
      m_currentVersionKnown(false),
      m_bytesPerSec(0),
      m_haveRateSample(false),
      m_signalKnown(false),
      m_signalDbm(0) {
  // Every member initialised, including the buffers: these objects are
  // heap-allocated in main.cpp and the heap is not zeroed (CLAUDE.md, testing
  // conventions - this has caused real bugs here before).
  m_version[0] = '\0';
  m_note[0] = '\0';
  m_currentVersion[0] = '\0';
  m_versionTransition[0] = '\0';
  m_signalDetail[0] = '\0';
  m_transferDetail[0] = '\0';
  m_etaDetail[0] = '\0';
  render(m_nowMs);
}

void OtaOutcome::begin(unsigned long nowMs) {
  m_result = OtaResult::InProgress;
  m_phase = OtaPhase::Connecting;
  m_code = 0;
  m_bytesDone = 0;
  m_bytesTotal = 0;
  m_lastProgressMs = nowMs;
  m_settledAtMs = nowMs;
  m_downloading = false;
  m_versionKnown = false;
  m_noticeCleared = false;
  m_restored = false;
  m_acked = false;
  m_nowMs = nowMs;
  m_version[0] = '\0';
  m_note[0] = '\0';

  // Per-attempt, same as m_version/m_versionKnown above: a retry must not
  // carry the previous attempt's version stamp, rate or signal forward.
  m_currentVersion[0] = '\0';
  m_currentVersionKnown = false;
  m_versionTransition[0] = '\0';
  m_bytesPerSec = 0;
  m_haveRateSample = false;
  m_signalKnown = false;
  m_signalDbm = 0;
  m_signalDetail[0] = '\0';
  m_transferDetail[0] = '\0';
  m_etaDetail[0] = '\0';

  render(m_nowMs);
}

void OtaOutcome::notePhase(OtaPhase phase, unsigned long nowMs) {
  m_nowMs = nowMs;
  if (settled()) {
    return;  // a settled outcome does not move again
  }
  m_phase = phase;
  if (phase == OtaPhase::Downloading) {
    // Arm the watchdog from the moment the transfer is supposed to start: a
    // transfer that never yields a first byte is the commonest stall of all.
    m_downloading = true;
    m_lastProgressMs = nowMs;
  } else {
    m_downloading = false;
  }
  render(nowMs);
}

void OtaOutcome::noteProgress(unsigned long bytesDone, unsigned long bytesTotal,
                              unsigned long nowMs) {
  if (settled()) {
    return;
  }
  m_nowMs = nowMs;
  if (bytesTotal > 0) {
    m_bytesTotal = bytesTotal;
  }
  if (bytesDone > m_bytesDone) {
    // Feed the rate EWMA from the SAME increase that feeds the stall
    // watchdog, before either of the two "previous" values below is
    // overwritten. See "RATE SMOOTHING" in otaoutcome.h: a deltaMs of 0 (two
    // calls in the same millisecond, including the very first call right
    // after notePhase(Downloading, ...) primed the clock at this same
    // instant) is skipped rather than divided by zero; the first valid
    // sample seeds the average outright rather than being blended in.
    const unsigned long deltaBytes = bytesDone - m_bytesDone;
    const unsigned long deltaMs = nowMs - m_lastProgressMs;
    if (deltaMs > 0) {
      const unsigned long sample = deltaBytes * 1000UL / deltaMs;
      if (!m_haveRateSample) {
        m_bytesPerSec = sample;  // nothing to smooth yet - seed outright
        m_haveRateSample = true;
      } else {
        m_bytesPerSec = (3UL * m_bytesPerSec + sample) / 4UL;
      }
    }
    m_bytesDone = bytesDone;
    m_lastProgressMs = nowMs;  // ONLY an increase counts as being alive
  }
  // render() every call, not just on an increase: the point of this is a
  // LIVE transfer line, and Update.onProgress can call in with an unchanged
  // count while time (and therefore bytesPerSec(nowMs)'s staleness window)
  // keeps moving. This is cheap - string formatting inside this object, no
  // cross-task publish - unlike ESPCommsManager's throttled republish to
  // GlobalState, which is where the real per-chunk cost would be.
  render(nowMs);
}

void OtaOutcome::noteVersionUnknown() {
  m_versionKnown = false;
  m_version[0] = '\0';
  updateVersionTransition();
  render(m_nowMs);
}

void OtaOutcome::noteVersion(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') {
    noteVersionUnknown();
    return;
  }
  copyInto(m_version, kVersionLen, tag);
  m_versionKnown = true;
  updateVersionTransition();
  render(m_nowMs);
}

void OtaOutcome::noteCurrentVersion(const char* version) {
  copyInto(m_currentVersion, kVersionLen, version);
  m_currentVersionKnown = m_currentVersion[0] != '\0';
  updateVersionTransition();
  // No render(): versionTransition() is its own accessor, drawn as its own
  // label (Part 3) - it does not feed headline()/detail(), so there is
  // nothing here for render() to refresh.
}

void OtaOutcome::updateVersionTransition() {
  if (m_currentVersionKnown && m_versionKnown) {
    snprintf(m_versionTransition, kVersionTransitionLen, "%s -> %s",
             m_currentVersion, m_version);
  } else {
    m_versionTransition[0] = '\0';
  }
}

void OtaOutcome::noteSignal(int rssiDbm) {
  m_signalDbm = rssiDbm;
  m_signalKnown = true;
  snprintf(m_signalDetail, kSignalLen, "Wi-Fi %d dBm", rssiDbm);
  render(m_nowMs);
}

void OtaOutcome::settle(OtaResult r, unsigned long nowMs, int code,
                        const char* note) {
  if (settled()) {
    // One-way door. The first diagnosis wins; an unwinding path must not
    // overwrite "stalled at 18%" with a generic "no server" on the way out.
    return;
  }
  m_result = r;
  m_phase = OtaPhase::Done;
  m_code = code;
  m_downloading = false;
  m_settledAtMs = nowMs;
  m_nowMs = nowMs;
  copyInto(m_note, kDetailLen, note);
  render(nowMs);
}

void OtaOutcome::succeed(unsigned long nowMs) {
  settle(OtaResult::Success, nowMs, 0, nullptr);
}

void OtaOutcome::upToDate(unsigned long nowMs) {
  settle(OtaResult::UpToDate, nowMs, 0, nullptr);
}

void OtaOutcome::fail(OtaResult reason, unsigned long nowMs, int code,
                      const char* note) {
  // Defensive: fail() is a failure path, so a caller that passes a non-failure
  // reason gets the safe reading, not a spurious success.
  if (reason == OtaResult::Success || reason == OtaResult::UpToDate ||
      reason == OtaResult::InProgress) {
    reason = OtaResult::BadImage;
  }
  settle(reason, nowMs, code, note);
}

bool OtaOutcome::failIfStalled(unsigned long nowMs) {
  if (!stalled(nowMs)) {
    return false;
  }
  fail(OtaResult::DownloadStalled, nowMs, 0, nullptr);
  return true;
}

bool OtaOutcome::failed() const {
  // Written as an exclusion so a result added later is a failure by default.
  if (m_result == OtaResult::InProgress) return false;
  if (m_result == OtaResult::Success) return false;
  if (m_result == OtaResult::UpToDate) return false;
  return true;
}

int OtaOutcome::percent() const {
  if (m_bytesTotal == 0) {
    return -1;
  }
  unsigned long p = (m_bytesDone * 100UL) / m_bytesTotal;
  if (p > 100UL) {
    p = 100UL;
  }
  return (int)p;
}

bool OtaOutcome::stalled(unsigned long nowMs) const {
  if (settled() || !m_downloading) {
    return false;
  }
  return (unsigned long)(nowMs - m_lastProgressMs) >= kStallTimeoutMs;
}

// --- Rate, ETA, transfer/signal formatting (Aug 2026) -----------------------

unsigned long OtaOutcome::bytesPerSec(unsigned long nowMs) const {
  // Same "poll with a fresh now" contract as stalled(nowMs): a caller
  // reading this after the object has gone quiet (nothing has called
  // noteProgress() with an increase for kRateStaleMs) gets an honest zero,
  // not the last number the EWMA happened to hold.
  if ((unsigned long)(nowMs - m_lastProgressMs) >= kRateStaleMs) {
    return 0;
  }
  return m_bytesPerSec;
}

bool OtaOutcome::rateSteady(unsigned long nowMs) const {
  return !settled() && bytesPerSec(nowMs) > 0;
}

long OtaOutcome::etaSeconds(unsigned long nowMs) const {
  if (!rateSteady(nowMs) || m_bytesTotal == 0) {
    return -1;
  }
  if (m_bytesDone >= m_bytesTotal) {
    return 0;  // a server that over-reports cannot produce a negative ETA
  }
  const unsigned long remaining = m_bytesTotal - m_bytesDone;
  return (long)(remaining / bytesPerSec(nowMs));
}

const char* OtaOutcome::transferDetail(unsigned long nowMs) const {
  if (m_bytesTotal == 0) {
    // Content length unknown (e.g. the home-network fallback URL) - a bytes-
    // of-total figure would be a lie, so show nothing rather than "?".
    m_transferDetail[0] = '\0';
    return m_transferDetail;
  }
  // Clamp for display exactly as percent() clamps to 100: a server that
  // over-reports cannot show a "done" bigger than "total".
  unsigned long done = m_bytesDone;
  if (done > m_bytesTotal) {
    done = m_bytesTotal;
  }
  const unsigned long doneTenths = tenthsOfMb(done);
  const unsigned long totalTenths = tenthsOfMb(m_bytesTotal);
  const unsigned long rate = bytesPerSec(nowMs);
  if (rate > 0) {
    snprintf(m_transferDetail, kTransferLen, "%lu.%lu/%lu.%luMB  %lukB/s",
             doneTenths / 10UL, doneTenths % 10UL, totalTenths / 10UL,
             totalTenths % 10UL, rate / 1000UL);
  } else {
    // No rate sample yet - bytes only, rather than a fake "0kB/s".
    snprintf(m_transferDetail, kTransferLen, "%lu.%lu/%lu.%luMB",
             doneTenths / 10UL, doneTenths % 10UL, totalTenths / 10UL,
             totalTenths % 10UL);
  }
  return m_transferDetail;
}

const char* OtaOutcome::etaDetail(unsigned long nowMs) const {
  const long secs = etaSeconds(nowMs);
  if (!rateSteady(nowMs) || secs < 0) {
    // This class would rather show nothing than a number it cannot stand
    // behind - see rateSteady()'s header comment.
    m_etaDetail[0] = '\0';
    return m_etaDetail;
  }
  if (secs >= 60) {
    snprintf(m_etaDetail, kEtaLen, "ETA %ldm %lds", secs / 60, secs % 60);
  } else {
    snprintf(m_etaDetail, kEtaLen, "ETA %lds", secs);
  }
  return m_etaDetail;
}

unsigned long OtaOutcome::holdMs() const {
  switch (m_result) {
    case OtaResult::Success:
      return kSuccessHoldMs;
    case OtaResult::UpToDate:
      return kInfoHoldMs;
    case OtaResult::InProgress:
      return 0;
    default:
      return kFailureHoldMs;
  }
}

OtaExit OtaOutcome::exitAction(unsigned long nowMs) const {
  if (!settled()) {
    return OtaExit::Waiting;
  }
  const unsigned long since = (unsigned long)(nowMs - m_settledAtMs);

  // A keypress is proof the outcome was seen, which is the only thing the hold
  // was ever buying. Release at once.
  if (!m_acked) {
    if (since < holdMs()) {
      return OtaExit::Waiting;
    }
    if (requiresAck()) {
      // Bounded wait: keep asking for the acknowledgement, but never park the
      // machine on a dialog forever. noticePending() carries the fact onward.
      if (since < kAckTimeoutMs) {
        return OtaExit::Waiting;
      }
      return OtaExit::ReturnToMachine;
    }
  }

  // The ONLY path to a reboot. A failure never reaches it, and neither does a
  // notice restored after one - that would be a boot loop.
  if (m_result == OtaResult::Success && !m_restored) {
    return OtaExit::RebootNow;
  }
  return OtaExit::ReturnToMachine;
}

bool OtaOutcome::noticePending(unsigned long nowMs) const {
  if (!settled() || m_noticeCleared || m_acked) {
    return false;
  }
  if (failed()) {
    return true;  // until somebody says they have seen it
  }
  if (m_result == OtaResult::Success && m_restored) {
    // The post-reboot "it worked" banner: seen once, then gone.
    return (unsigned long)(nowMs - m_settledAtMs) < kNoticeMs;
  }
  return false;
}

OtaNotice OtaOutcome::snapshot() const {
  OtaNotice n;
  n.magic = kNoticeMagic;
  n.result = (uint8_t)m_result;
  n.pad = 0;
  n.code = (int16_t)m_code;
  n.bytesDone = (uint32_t)m_bytesDone;
  n.bytesTotal = (uint32_t)m_bytesTotal;
  copyInto(n.version, kVersionLen, m_version);
  return n;
}

bool OtaOutcome::noticeValid(const OtaNotice& n) {
  if (n.magic != kNoticeMagic) {
    return false;
  }
  if (n.result == (uint8_t)OtaResult::InProgress) {
    return false;  // never a thing worth carrying across a boot
  }
  return n.result <= (uint8_t)OtaResult::BadImage;
}

void OtaOutcome::restore(const OtaNotice& n, unsigned long nowMs) {
  if (!noticeValid(n)) {
    return;
  }
  m_result = (OtaResult)n.result;
  m_phase = OtaPhase::Done;
  m_code = n.code;
  m_bytesDone = n.bytesDone;
  m_bytesTotal = n.bytesTotal;
  m_lastProgressMs = nowMs;
  m_settledAtMs = nowMs;
  m_downloading = false;
  m_noticeCleared = false;
  m_acked = false;
  m_restored = true;
  copyInto(m_version, kVersionLen, n.version);
  m_versionKnown = m_version[0] != '\0';
  m_note[0] = '\0';
  m_nowMs = nowMs;

  // None of these travel in OtaNotice (it is the pre-reboot struct, blitted
  // before anything about rate/signal/the running version existed) - a
  // restored outcome never had a live transfer or radio reading, and
  // m_versionTransition would only ever be "" here anyway since
  // noteCurrentVersion() is never called on the new firmware's boot path.
  m_currentVersion[0] = '\0';
  m_currentVersionKnown = false;
  m_versionTransition[0] = '\0';
  m_bytesPerSec = 0;
  m_haveRateSample = false;
  m_signalKnown = false;
  m_signalDbm = 0;
  m_signalDetail[0] = '\0';

  render(nowMs);
}

const char* OtaOutcome::resultName(OtaResult r) {
  switch (r) {
    case OtaResult::InProgress: return "InProgress";
    case OtaResult::Success: return "Success";
    case OtaResult::UpToDate: return "UpToDate";
    case OtaResult::NoNetwork: return "NoNetwork";
    case OtaResult::NoServer: return "NoServer";
    case OtaResult::DownloadStalled: return "DownloadStalled";
    case OtaResult::BadImage: return "BadImage";
  }
  return "?";
}

// The whole point of the feature is here: three unmistakably different
// headlines, and a detail line that names the operator's next move rather than
// the library call that returned false.
void OtaOutcome::render(unsigned long nowMs) {
  switch (m_result) {
    case OtaResult::InProgress:
      switch (m_phase) {
        case OtaPhase::Connecting:
          copyInto(m_headline, kHeadlineLen, "CONNECTING");
          // The PHASE WORD, not the signal. The signal is on the context
          // line (contextLine()) because a short detail gets promoted into
          // the headline slot by otaFittedLine() and would replace
          // "CONNECTING" entirely - the rendered screenshot is what caught
          // that; every static_assert in the display passed regardless.
          copyInto(m_detail, kDetailLen, "Joining Wi-Fi");
          break;
        case OtaPhase::Checking:
          copyInto(m_headline, kHeadlineLen, "CHECKING");
          copyInto(m_detail, kDetailLen, "Looking for a newer release");
          break;
        case OtaPhase::Downloading: {
          copyInto(m_headline, kHeadlineLen, "UPDATING");
          // Bytes + rate + ETA while it is actually happening - "Downloading"
          // on its own repeats the headline and wastes the slot. Degrades in
          // stages exactly as transferDetail()/etaDetail() do: no content
          // length yet -> "Downloading"; bytes known, no rate sample yet ->
          // bytes only; rate known -> bytes + rate [+ ETA once steady].
          const char* xfer = transferDetail(nowMs);
          const char* eta = etaDetail(nowMs);
          if (xfer[0] == '\0') {
            copyInto(m_detail, kDetailLen, "Downloading");
          } else if (eta[0] != '\0') {
            snprintf(m_detail, kDetailLen, "%s  %s", xfer, eta);
          } else {
            copyInto(m_detail, kDetailLen, xfer);
          }
          break;
        }
        case OtaPhase::Finishing:
          copyInto(m_headline, kHeadlineLen, "UPDATING");
          copyInto(m_detail, kDetailLen, "Writing image");
          break;
        default:
          copyInto(m_headline, kHeadlineLen, "UPDATE");
          m_detail[0] = '\0';
          break;
      }
      return;

    case OtaResult::Success:
      copyInto(m_headline, kHeadlineLen, "UPDATED");
      if (m_versionKnown) {
        // m_restored is false for the whole 2 s (kSuccessHoldMs) this is on
        // screen BEFORE ESP.restart() actually runs, on the OLD firmware -
        // "Now running" is a lie until then. restore() is the only thing
        // that ever sets m_restored, on the NEW firmware's first screen,
        // which is the only moment "Now running" is true.
        if (m_restored) {
          snprintf(m_detail, kDetailLen, "Now running %s", m_version);
        } else {
          snprintf(m_detail, kDetailLen, "Restarting into %s", m_version);
        }
      } else {
        copyInto(m_detail, kDetailLen, "New image installed");
      }
      return;

    case OtaResult::UpToDate:
      copyInto(m_headline, kHeadlineLen, "UP TO DATE");
      if (m_versionKnown) {
        snprintf(m_detail, kDetailLen, "Already on %s", m_version);
      } else {
        copyInto(m_detail, kDetailLen, "Nothing to install");
      }
      return;

    default:
      break;
  }

  // Every failure shares one headline, because the one thing the operator must
  // read from across the shop is that this did NOT work.
  copyInto(m_headline, kHeadlineLen, "UPDATE FAILED");

  switch (m_result) {
    case OtaResult::NoNetwork:
      copyInto(m_detail, kDetailLen, "No Wi-Fi - check network settings");
      break;

    case OtaResult::NoServer:
      if (m_code != 0) {
        snprintf(m_detail, kDetailLen, "No server (HTTP %d)", m_code);
      } else {
        copyInto(m_detail, kDetailLen, "Server unreachable");
      }
      break;

    case OtaResult::DownloadStalled: {
      const int pc = percent();
      if (pc >= 0 && m_bytesTotal > 0) {
        const unsigned long t = tenthsOfMb(m_bytesTotal);
        // "Stalled at 18% of 1.5 MB" - the percentage is the diagnosis here.
        // A hang at a repeatable fraction is the modem-sleep fault; a hang at a
        // random one is the link. Neither is visible from "Update failed".
        snprintf(m_detail, kDetailLen, "Stalled at %d%% of %lu.%lu MB", pc,
                 t / 10UL, t % 10UL);
      } else {
        copyInto(m_detail, kDetailLen, "Download stalled");
      }
      break;
    }

    case OtaResult::BadImage:
      if (m_note[0] != '\0') {
        snprintf(m_detail, kDetailLen, "Bad image: %s", m_note);
      } else {
        copyInto(m_detail, kDetailLen, "Bad image - not installed");
      }
      break;

    default:
      copyInto(m_detail, kDetailLen, "Update failed");
      break;
  }

  // A failure while the release tag was never confirmed gets said out loud
  // where there is room: otherwise "no server" reads as "the download server",
  // when what actually happened may be that nothing was ever checked.
  if (!m_versionKnown && m_result == OtaResult::NoServer &&
      strlen(m_detail) + strlen(" - version unchecked") < (size_t)kDetailLen) {
    strcat(m_detail, " - version unchecked");
  }
}
