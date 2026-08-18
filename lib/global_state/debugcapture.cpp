#include "debugcapture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default allocator. Overridable only by the host tests, which need to drive
// the size ladder and the out-of-memory path deterministically (a real malloc
// on the host never fails at these sizes).
void* (*DebugCapture::s_alloc)(size_t) = malloc;
void (*DebugCapture::s_free)(void*) = free;

void DebugCapture::setAllocatorForTest(void* (*alloc)(size_t),
                                       void (*dealloc)(void*)) {
  s_alloc = (alloc != 0) ? alloc : malloc;
  s_free = (dealloc != 0) ? dealloc : free;
}

// EVERY member is initialised here. GlobalState is heap-allocated (`new` in
// main.cpp) and the heap is not zeroed - CLAUDE.md records real bugs from
// exactly this (dead buttons, garbage sync state), and the members below are
// the ones the spindle hot loop dereferences.
DebugCapture::DebugCapture() {
  m_write = 0;
  m_buffer = 0;
  m_reserve = 0;
  m_recording = false;
  m_state = DBG_OFF;
  m_count = 0;
  m_capacity = 0;
  m_startMicros = 0;
  m_lastSampleMicros = 0;
  m_lastDirection = 0;
  m_lastIterationMicros = 0;
  m_peakGapUs = 0;
  m_peakDelta = 0;
  m_peakDeltaMagnitude = 0;
}

DebugCapture::~DebugCapture() { discard(); }

bool DebugCapture::arm(uint32_t startMicros) {
  // Free anything already held first, so re-arming is a restart rather than a
  // leak. discard() clears m_recording before it frees.
  discard();

  // The size ladder. See kWantSamples in the header for the DRAM arithmetic:
  // the trace stays resident while the upload runs, so leaving the upload
  // enough heap matters more than the last few seconds of window.
  static const int ladder[] = {kWantSamples, 1600, 1000, kMinSamples};
  DebugData* buffer = 0;
  int capacity = 0;
  // Each rung must satisfy BOTH allocations: the trace, and the reserve the
  // upload will need afterwards. Taking a rung that fits the trace but leaves
  // no contiguous room for the upload task is the failure kUploadReserveBytes
  // documents - the trace is worthless if it can never be sent. Ordering
  // matters: the trace is claimed first so the reserve cannot take the block
  // the trace would have used and push it down a rung unnecessarily.
  void* reserve = 0;
  for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
    buffer = (DebugData*)s_alloc((size_t)ladder[i] * sizeof(DebugData));
    if (buffer == 0) {
      continue;
    }
    reserve = s_alloc(kUploadReserveBytes);
    if (reserve != 0) {
      capacity = ladder[i];
      break;
    }
    // Trace fits, upload would not. Give it back and try a shorter window.
    s_free(buffer);
    buffer = 0;
  }
  if (buffer == 0) {
    m_state = DBG_NOMEM;
    return false;
  }

  m_buffer = buffer;
  m_reserve = reserve;
  m_capacity = capacity;
  m_count = 0;
  m_startMicros = startMicros;
  m_lastDirection = 0;
  // Seeded a full interval in the past so the FIRST update() after arming
  // records, rather than the trace opening with a 25 ms hole.
  m_lastSampleMicros = startMicros - kSampleIntervalMicros;
  // Seeded to NOW, so the first iteration's gap is the time from arming to that
  // iteration - a few microseconds - and not a spurious multi-second "stall"
  // measured from whenever this object was constructed.
  m_lastIterationMicros = startMicros;
  m_peakGapUs = 0;
  m_peakDelta = 0;
  m_peakDeltaMagnitude = 0;
  m_write = buffer;

  // LAST, and deliberately so: the SpindleTask may be inside update() right
  // now, and this is the store that lets it start writing. Everything it will
  // touch is already in place above it.
  m_state = DBG_RECORDING;
  m_recording = true;
  return true;
}

void DebugCapture::discard() {
  // Stop the hot loop FIRST. Once this store lands, update() takes the
  // recording() == false path and never looks at m_write again, so the free
  // below cannot pull the buffer out from under a write in progress.
  m_recording = false;
  m_write = 0;

  if (m_buffer != 0) {
    s_free(m_buffer);
    m_buffer = 0;
  }
  releaseUploadReserve();
  m_capacity = 0;
  m_count = 0;
  m_state = DBG_OFF;
}

// Hands the upload its memory back. Deliberately NOT part of release()/
// discard() ordering games: it touches nothing the hot loop reads, so it needs
// no m_recording dance. Idempotent, because a failed upload keeps its trace and
// the retry path reaches here a second time.
void DebugCapture::releaseUploadReserve() {
  if (m_reserve != 0) {
    s_free(m_reserve);
    m_reserve = 0;
  }
}

void DebugCapture::release(DebugCaptureState endState) {
  // Same ordering rule as discard(): stop the hot loop, then free.
  m_recording = false;
  m_write = 0;
  if (m_buffer != 0) {
    s_free(m_buffer);
    m_buffer = 0;
  }
  releaseUploadReserve();
  // m_count and m_capacity are deliberately KEPT: they are what the status
  // line reports ("SENT 2400"). readyToSend() tests m_buffer, so a released
  // capture can never be uploaded a second time.
  m_state = endState;
}

// --- Operator-visible status ------------------------------------------------

void formatCaptureStatus(char* out, size_t n, int state, int count,
                         int capacity) {
  if (out == 0 || n == 0) {
    return;
  }
  switch (state) {
  case DBG_RECORDING:
    // The one line that changes while the machine is cutting. Both numbers,
    // not a percentage: "how much have I got" and "how much is there" are
    // different questions when the size ladder may have handed out a smaller
    // buffer than asked for.
    snprintf(out, n, "REC %d/%d", count, capacity);
    return;
  case DBG_FULL:
    // The instruction, not the state. Nothing will be sent until the carriage
    // is at rest, and the operator is the only one who can arrange that.
    snprintf(out, n, "FULL: STOP TO SEND");
    return;
  case DBG_SENDING:
    snprintf(out, n, "SENDING TRACE");
    return;
  case DBG_SENT:
    snprintf(out, n, "SENT %d", count);
    return;
  case DBG_FAILED:
    // The trace is still in RAM; stopping the carriage again retries.
    snprintf(out, n, "SEND FAILED");
    return;
  case DBG_NOMEM:
    snprintf(out, n, "CAPTURE: NO MEMORY");
    return;
  case DBG_OFF:
  default:
    // No capture: the screen is just the Diagnostics screen, and says so.
    snprintf(out, n, "DIAGNOSTICS");
    return;
  }
}

// --- Wire format -----------------------------------------------------------

// The two starvation columns are APPENDED, so the original eleven keep their
// positions and any capture taken before them still parses by column index.
// analyse_capture.py accepts both widths for the same reason.
const char* debugCsvHeader() {
  return "time,posError,posErrorRaw,pulseToTarget,pos,expectedPos,speed,"
         "direction,targetSpeed,speedDiff,timeToTarget,loopGapUs,spindleDelta";
}

// COLUMN ORDER FOLLOWS THE HEADER, NOT THE STRUCT. `direction` is column 8
// even though m_currentDirection is the sixth member - that is how the serial
// dump this replaces was written, and the header above is the one the analysis
// script and any capture the owner already has both expect. Do not "tidy" one
// without the other.
//
// %g rather than %f: six significant digits keeps a row near 80 bytes instead
// of 130 and loses nothing that matters at these magnitudes, while still
// printing small values (m_timeToTarget is often < 0.01 s) properly, which a
// fixed %.4f would flatten to zero.
int formatDebugRow(char* out, size_t n, const DebugData& d) {
  const int written =
      snprintf(out, n, "%d,%g,%g,%g,%d,%g,%g,%d,%g,%g,%g,%d,%d", d.tm,
               (double)d.positionError, (double)d.positionErrorRaw,
               (double)d.pulsesToTargetSpeed, d.m_currentPosition,
               (double)d.m_expectedPosition, (double)d.m_leadscrewSpeed,
               d.m_currentDirection, (double)d.m_targetSpeed,
               (double)d.m_speedDif, (double)d.m_timeToTarget, d.loopGapUs,
               d.spindleDelta);
  if (written < 0 || (size_t)written >= n) {
    return -1;
  }
  return written;
}

// --- URL parsing -----------------------------------------------------------

bool parseHttpUrl(const char* url, HttpUrlParts& out) {
  // Initialise the whole output first: a caller that ignores the false return
  // must not be handed a half-filled struct with a stale host in it.
  out.secure = false;
  out.host[0] = '\0';
  out.port = 0;
  out.path[0] = '\0';

  if (url == 0) {
    return false;
  }

  const char* p;
  if (strncmp(url, "http://", 7) == 0) {
    out.secure = false;
    out.port = 80;
    p = url + 7;
  } else if (strncmp(url, "https://", 8) == 0) {
    out.secure = true;
    out.port = 443;
    p = url + 8;
  } else {
    return false;
  }

  // Authority runs to the first '/', '?' or end. No userinfo ('@') support and
  // no IPv6 literals: the sink is a host the owner runs, and silently
  // mis-parsing something more exotic would be worse than refusing it.
  const char* authEnd = p;
  while (*authEnd != '\0' && *authEnd != '/' && *authEnd != '?') {
    authEnd++;
  }
  if (authEnd == p) {
    return false;  // empty host
  }
  if (memchr(p, '@', (size_t)(authEnd - p)) != 0) {
    return false;
  }

  const char* colon = 0;
  for (const char* c = p; c < authEnd; c++) {
    if (*c == ':') {
      colon = c;
    }
  }

  const char* hostEnd = (colon != 0) ? colon : authEnd;
  const size_t hostLen = (size_t)(hostEnd - p);
  if (hostLen == 0 || hostLen >= sizeof(out.host)) {
    return false;
  }
  memcpy(out.host, p, hostLen);
  out.host[hostLen] = '\0';

  if (colon != 0) {
    int port = 0;
    const char* c = colon + 1;
    if (c == authEnd) {
      return false;  // "host:" with no number
    }
    for (; c < authEnd; c++) {
      if (*c < '0' || *c > '9') {
        return false;
      }
      port = port * 10 + (*c - '0');
      if (port > 65535) {
        return false;
      }
    }
    if (port == 0) {
      return false;
    }
    out.port = port;
  }

  if (*authEnd == '\0') {
    out.path[0] = '/';
    out.path[1] = '\0';
    return true;
  }
  // A URL may go straight from the authority to a query ("host?a=b"), in which
  // case the request line still needs an origin-form path - "/?a=b", not
  // "?a=b", which no server would route.
  const size_t prefix = (*authEnd == '?') ? 1 : 0;
  const size_t pathLen = strlen(authEnd);
  if (pathLen + prefix >= sizeof(out.path)) {
    return false;
  }
  if (prefix != 0) {
    out.path[0] = '/';
  }
  memcpy(out.path + prefix, authEnd, pathLen + 1);
  return true;
}
