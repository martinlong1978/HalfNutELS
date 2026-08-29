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
      m_acked(false) {
  // Every member initialised, including the buffers: these objects are
  // heap-allocated in main.cpp and the heap is not zeroed (CLAUDE.md, testing
  // conventions - this has caused real bugs here before).
  m_version[0] = '\0';
  m_note[0] = '\0';
  render();
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
  m_version[0] = '\0';
  m_note[0] = '\0';
  render();
}

void OtaOutcome::notePhase(OtaPhase phase, unsigned long nowMs) {
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
  render();
}

void OtaOutcome::noteProgress(unsigned long bytesDone, unsigned long bytesTotal,
                              unsigned long nowMs) {
  if (settled()) {
    return;
  }
  if (bytesTotal > 0) {
    m_bytesTotal = bytesTotal;
  }
  if (bytesDone > m_bytesDone) {
    m_bytesDone = bytesDone;
    m_lastProgressMs = nowMs;  // ONLY an increase counts as being alive
  }
}

void OtaOutcome::noteVersionUnknown() {
  m_versionKnown = false;
  m_version[0] = '\0';
  render();
}

void OtaOutcome::noteVersion(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') {
    noteVersionUnknown();
    return;
  }
  copyInto(m_version, kVersionLen, tag);
  m_versionKnown = true;
  render();
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
  copyInto(m_note, kDetailLen, note);
  render();
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
  render();
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
void OtaOutcome::render() {
  switch (m_result) {
    case OtaResult::InProgress:
      switch (m_phase) {
        case OtaPhase::Connecting:
          copyInto(m_headline, kHeadlineLen, "CONNECTING");
          copyInto(m_detail, kDetailLen, "Joining Wi-Fi");
          break;
        case OtaPhase::Checking:
          copyInto(m_headline, kHeadlineLen, "CHECKING");
          copyInto(m_detail, kDetailLen, "Looking for a newer release");
          break;
        case OtaPhase::Downloading:
          copyInto(m_headline, kHeadlineLen, "UPDATING");
          copyInto(m_detail, kDetailLen, "Downloading");
          break;
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
        snprintf(m_detail, kDetailLen, "Now running %s", m_version);
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
