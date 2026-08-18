// Motion-trace capture buffer for the leadscrew hot loop.
//
// WHY THIS EXISTS. After the Mk2 branch the owner reports a thread-cutting
// regression on the real lathe: frequent small (mostly audible) jitters, and a
// big glitch every 2-10 s where the leadscrew reverses about 180 degrees
// rapidly and then carries on. Neither was present before. This class is the
// instrument for finding it: it records a compact trace of the planner's state
// from inside Leadscrew::update(), and the trace is later POSTed to a server
// the owner runs (tools/debugsink).
//
// THE INSTRUMENT MUST NOT PERTURB WHAT IT MEASURES. Two rules follow, and both
// are load-bearing:
//
//   1. WHEN DISABLED, IT COSTS ONE VOLATILE BOOL LOAD AND A BRANCH. Nothing
//      else. recording() is the whole of the disabled path; every counter,
//      every timestamp compare and the buffer-full test all live INSIDE that
//      branch. (The code this replaced was worse: it ran an unconditional
//      pointer subtraction, multiply and compare on every pulse, outside the
//      getDebugMode() guard - see the note on commit() below.)
//   2. NO ALLOCATION, NO LOCKS, NO BLOCKING CALLS in anything update() calls.
//      arm() and discard() allocate and free; they are called from the
//      DisplayTask (a menu press), never from the SpindleTask.
//
// PURE C++ ON PURPOSE: no Arduino, ESP or FreeRTOS includes, so the decimator,
// the buffer bookkeeping, the CSV formatter and the URL parser are all built
// and tested by `pio test -e native` (test/test_debugcapture).
#ifndef ELS_DEBUGCAPTURE_H
#define ELS_DEBUGCAPTURE_H

#include <stddef.h>
#include <stdint.h>

// One captured sample. The field ORDER and the CSV column order below are the
// wire format the analysis script parses (tools/debugsink/analyse_capture.py);
// the header string is unchanged from the serial-dump version this replaced, so
// old captures and new ones read the same way.
//
// `tm` is MICROSECONDS SINCE THE CAPTURE WAS ARMED, not raw micros(). Raw
// micros() wraps a 32-bit int every ~35 minutes and the old dump stored it
// truncated to `int`, so a trace could contain a discontinuity that looked
// exactly like the glitch being hunted. Relative time cannot: a capture spans
// about a minute, and int32 microseconds hold 2147 seconds.
typedef struct DebugData {
  int tm;
  float positionError;
  float positionErrorRaw;
  float pulsesToTargetSpeed;
  int m_currentPosition;
  int m_currentDirection;
  float m_expectedPosition;
  float m_leadscrewSpeed;
  float m_targetSpeed;
  float m_speedDif;
  float m_timeToTarget;

  // --- The starvation pair (appended; 8 bytes per sample) -------------------
  //
  // THE HYPOTHESIS THESE TWO EXIST TO CONFIRM OR KILL. The symptom is a large
  // FORWARD jump every 2-10 s followed by a correction. If SpindleTask is being
  // starved, then: it misses iterations -> spindle counts pile up in the
  // encoder -> the next consumePosition() returns a big delta ->
  // m_expectedPosition leaps forward by delta x ratio -> the leadscrew rushes
  // to catch up. Every step of that chain is observable, and the two numbers
  // below are the two that were previously invisible.
  //
  //   loopGapUs     - microseconds between hot-loop iterations. The LARGEST gap
  //                   seen since the previous recorded sample, so decimation
  //                   cannot hide a stall (and on a sample FORCED by a stall it
  //                   is that stall's own gap - see DebugCapture::due()).
  //   spindleDelta  - what consumePosition() returned. The largest-magnitude
  //                   one since the previous sample, on the same reasoning.
  //
  // Read against positionError on one time axis they are decisive: a gap spike,
  // then a delta spike, then an error spike, IS starvation and nothing else. An
  // error spike with a flat gap and a flat delta means the fault is in the
  // motion maths instead, and the re-sync gate becomes the prime suspect.
  //
  // This replaces the serial-only ELS_STALL_TELEMETRY block that used to live
  // in src/main.cpp. That printed once a second over USB, which is unreadable
  // at the lathe - and the lathe is the only place the bug reproduces, because
  // the bench device has no spindle and no cutting load. Everything needed must
  // travel in this payload; nothing may depend on someone watching a terminal.
  int loopGapUs;
  int spindleDelta;
} DebugData;

// What the capture is doing right now. Written by three different tasks (the
// DisplayTask arms/discards, the SpindleTask fills, the upload task sends) and
// read by the display, so GlobalState holds it `volatile` with no lock - see
// CLAUDE.md on cross-task state.
enum DebugCaptureState {
  DBG_OFF = 0,     // no buffer allocated, nothing recorded
  DBG_RECORDING,   // armed: the buffer is allocated and filling
  DBG_FULL,        // buffer full; waiting for the carriage to be at rest
  DBG_SENDING,     // the upload task is POSTing it
  DBG_SENT,        // uploaded; the buffer has been freed
  DBG_FAILED,      // upload failed. The buffer is KEPT so it can be retried
  DBG_NOMEM        // arm() could not get a buffer even at its smallest size
};

// --- Capture rate ----------------------------------------------------------
//
// THE DECIMATION DECISION, in one place so it can be argued with.
//
// The predecessor recorded every 11th emitted step pulse. That is wrong for
// this hunt in both directions at once:
//   * it is tied to the PULSE TRAIN, so the sample rate scales with carriage
//     speed and drops to nothing exactly when the axis stalls - and a stall is
//     one of the things worth seeing;
//   * at a realistic threading rate (~1600 pulses/s) it samples ~145 times a
//     second, which fills any buffer that fits in DRAM in under 20 seconds.
//     The glitch being hunted recurs every 2-10 s, so a 17-second window might
//     contain two of them and might contain none.
//
// So: a PERIODIC sample every kSampleIntervalMicros of real time (uniform in
// time, independent of speed, and it keeps ticking while the axis is held),
// plus FORCED samples on the three things that must never be missed -
// rate-limited to kBurstGapMicros so none of them can empty the buffer
// instantly:
//
//   1. the commanded DIRECTION differs from the last sample's;
//   2. a hot-loop iteration GAP of kStallGapMicros or more;
//   3. a SPINDLE DELTA of kSpindleDeltaTrigger counts or more.
//
// (1) is what makes a reversal legible. A 180-degree reversal is ~400 leadscrew
// pulses, ~250 ms at speed: the periodic rate alone would catch it, but not its
// EDGES, and the edge timestamps are what turn "it reverses sometimes" into an
// interval and a trigger condition.
//
// (2) and (3) are what make a STALL legible, and they are why the periodic rate
// alone is not enough: a 2 ms starvation gap falls between two 25 ms samples
// far more often than not. With these, the stall forces its own sample within
// kBurstGapMicros, timestamped, with its own gap and delta in it. Belt and
// braces: even a stall that somehow does not force a sample still cannot be
// hidden, because loopGapUs and spindleDelta are PEAK-HELD between samples (see
// noteIteration()) rather than instantaneous.
//
// The sample site was also moved OUT of the "a pulse was just emitted" branch
// for the same reason - see the capture block in Leadscrew::update(). A stall
// while the axis is held by the re-sync gate emits no pulses at all, and that
// is precisely a case worth seeing.
//
// 25 ms x 2000 samples = 50 seconds, i.e. five to twenty-five glitches at the
// reported 2-10 s period. If instead the direction chatters continuously, or
// the loop stalls continuously, the 1 ms floor lets that consume the buffer in
// as little as 2 s - which is the correct outcome, because a trace full of
// chatter or stalls IS the bug.
//
// TIMEBASE. Everything below works in uint32_t microseconds, because that is
// what micros() actually returns on both the ESP32 and the host stub. Widening
// it to int64_t (as Leadscrew::update() does locally) does not undo the wrap,
// it just relocates it: a capture spanning the ~71-minute rollover would then
// see `now < start` and produce one absurd interval that reads exactly like
// the glitch being hunted. Unsigned differences are wrap-correct, so they are
// what the decimator and the relative clock both use.
static const uint32_t kSampleIntervalMicros = 25000;  // 40 Hz baseline
static const uint32_t kBurstGapMicros = 1000;         // 1 kHz cap on forced samples

// A hot-loop gap worth forcing a sample for. Same threshold the serial-only
// telemetry this replaces used: at the default config one leadscrew pulse
// interval at full jog is ~79 us, so 2 ms is already ~25 pulses' worth of
// missed iterations.
static const uint32_t kStallGapMicros = 2000;

// A spindle delta worth forcing a sample for. Normal is 0 or 1: the loop runs
// far faster than the encoder produces counts (1200 PPR at 300 rpm is 6000
// counts/s, i.e. one count per 167 us against a loop period of a few us). Eight
// counts have therefore already piled up behind something.
static const int kSpindleDeltaTrigger = 8;

// --- Buffer size -----------------------------------------------------------
//
// sizeof(DebugData) is 52 (13 x 4 - the starvation pair costs 8 bytes a
// sample). kWantSamples x 52 = 104,000 bytes.
//
// The sample count came down from 2400 to 2000 to pay for those 8 bytes at the
// SAME memory footprint, rather than growing the allocation: the DRAM
// arithmetic below is what keeps the upload possible at all, and 50 seconds
// still spans five to twenty-five of the reported glitches. Window length was
// the right thing to spend, and the two new fields are the two that decide the
// question.
//
// The ESP32-WROOM has ~320 KB of DRAM and this firmware already uses ~115 KB,
// so ~200 KB is free at rest. The buffer is still resident while the upload
// runs (that is what is being uploaded), and the upload needs WiFi plus, for an
// https sink, a TLS session and the CA bundle - call it 45 KB peak on top of a
// 16 KB task stack. 105 KB for the trace leaves ~80 KB for that, which is why
// the ladder below exists rather than a single fixed size: if the heap is more
// fragmented than this arithmetic assumes, arm() takes the next size down and
// says so, instead of failing outright or - worse - succeeding and then
// starving the upload.
//
// Prefer an http:// sink over https:// (see tools/debugsink/README.md): it
// removes the TLS session and the CA bundle from the peak entirely.
static const int kWantSamples = 2000;   // 50 s at the periodic rate
static const int kMinSamples = 500;     // 12.5 s - the smallest useful window

class DebugCapture {
 public:
  DebugCapture();
  ~DebugCapture();

  // ------------------------------------------------------------------
  // HOT PATH. Everything here is header-inline, branch-light and free of
  // allocation, division and library calls. Called from Leadscrew::update()
  // on the SpindleTask (core 0).
  // ------------------------------------------------------------------

  // The whole of the disabled path. False whenever no capture is running, and
  // it is the ONLY thing update() evaluates in that case.
  //
  // It is also the guarantee that slot() is safe to dereference: m_recording is
  // set true only after a successful allocation in arm(), and cleared before
  // the buffer is freed in discard() and before commit() lets the write cursor
  // reach the end. The pointers it protects used to be public members of
  // GlobalState that nothing ever allocated - re-enabling the old debug toggle
  // would have written through a null cursor from the spindle hot loop.
  inline bool recording() const { return m_recording; }

  // The slot currently being filled. Valid only while recording() is true.
  //
  // Two separate places in update() write into it - getTargetSpeedDistanceInPulses()
  // fills the five speed fields near the top, the pulse path fills the six
  // position fields near the bottom - and only the second one commits. Writes
  // into an uncommitted slot are simply overwritten next time round, and both
  // halves of a slot that IS committed come from the same update() iteration.
  inline DebugData* slot() const { return m_write; }

  // ONE PER HOT-LOOP ITERATION, called before the slot is filled. Measures the
  // gap since the previous iteration and peak-holds it, and the spindle delta,
  // until the next commit().
  //
  // PEAK-HOLD, NOT INSTANTANEOUS, and that is the point: the trace is decimated
  // ~1000:1, so an instantaneous reading would report whichever iteration
  // happened to be sampled and quietly drop the 2 ms stall next to it. Held
  // this way, loopGapUs is "the worst the loop did since the last row" - a
  // stall cannot be absent from the record, only attributed to a window rather
  // than an instant. On a sample FORCED by that stall (see due()) the window is
  // the stall itself, so the two coincide exactly.
  //
  // Returns the instantaneous gap, for callers that want it.
  inline uint32_t noteIteration(uint32_t nowMicros, int spindleDelta) {
    const uint32_t gap = nowMicros - m_lastIterationMicros;  // wrap-correct
    m_lastIterationMicros = nowMicros;
    if (gap > m_peakGapUs) {
      m_peakGapUs = gap;
    }
    const int magnitude = (spindleDelta < 0) ? -spindleDelta : spindleDelta;
    if (magnitude > m_peakDeltaMagnitude) {
      m_peakDeltaMagnitude = magnitude;
      m_peakDelta = spindleDelta;
    }
    return gap;
  }

  // The peak-held pair, for the two fields of the pending sample.
  inline int peakGapUs() const { return (int)m_peakGapUs; }
  inline int peakSpindleDelta() const { return m_peakDelta; }

  // Has something worth recording happened since the last sample? True once a
  // stall-sized gap or a piled-up spindle delta has been seen, and stays true
  // until commit() clears the peaks - so the event is recorded within
  // kBurstGapMicros even if it lands mid-interval.
  inline bool disturbed() const {
    return m_peakGapUs >= kStallGapMicros ||
           m_peakDeltaMagnitude >= kSpindleDeltaTrigger;
  }

  // Should this update() iteration be recorded? See the decimation note above.
  //
  // `direction` is the leadscrew's commanded direction as an int (-1/0/+1).
  inline bool due(uint32_t nowMicros, int direction) const {
    const uint32_t since = nowMicros - m_lastSampleMicros;  // wrap-correct
    if (direction != m_lastDirection || disturbed()) {
      return since >= kBurstGapMicros;
    }
    return since >= kSampleIntervalMicros;
  }

  // Publish the filled slot and advance. Called ONLY when due() said yes.
  //
  // The buffer-full test lives here, inside the recording() branch. It used to
  // sit outside the debug guard in leadscrew.cpp, costing two volatile pointer
  // loads, a subtraction, a multiply and a compare on every single pulse of
  // every cut, whether or not anyone was debugging.
  //
  // Filling the buffer STOPS the capture (m_recording false, state DBG_FULL);
  // it never wraps. A ring buffer would be the wrong instrument here: the
  // interesting event is not always the last one, and an interval between
  // glitches cannot be measured from a window that has already discarded the
  // previous one.
  inline void commit(uint32_t nowMicros, int direction) {
    m_lastSampleMicros = nowMicros;
    m_lastDirection = direction;
    // Open a fresh peak-hold window. The values just written into the slot
    // describe the window that ends here.
    m_peakGapUs = 0;
    m_peakDelta = 0;
    m_peakDeltaMagnitude = 0;
    m_count = m_count + 1;
    if (m_count >= m_capacity) {
      m_recording = false;
      m_state = DBG_FULL;
      return;
    }
    m_write = m_write + 1;
  }

  // Microseconds since arm(), for the `tm` column. One subtraction, in
  // unsigned arithmetic so it stays correct across the micros() rollover.
  inline int32_t relativeMicros(uint32_t nowMicros) const {
    return (int32_t)(nowMicros - m_startMicros);
  }

  // ------------------------------------------------------------------
  // COLD PATH. Menu presses (DisplayTask) and the upload task only.
  // ------------------------------------------------------------------

  // Allocate and start recording. Tries kWantSamples, then progressively
  // smaller buffers down to kMinSamples, so a fragmented heap costs window
  // length rather than the whole capture. Returns false (state DBG_NOMEM) if
  // even the smallest allocation fails.
  //
  // `startMicros` seeds the relative clock and the decimator; pass micros().
  // Any buffer already held is freed first, so arming twice is safe.
  bool arm(uint32_t startMicros);

  // Stop, free the buffer, back to DBG_OFF. Safe to call in any state; the
  // recording flag is cleared BEFORE the free, so the hot loop can never be
  // between recording() and slot() on a buffer that is going away.
  void discard();

  // Free the trace but KEEP the sample count and land in `endState`, so the
  // Diagnostics screen can still say "SENT 2400" after the upload has handed
  // the 100 KB back to the heap. readyToSend() goes false because the buffer
  // is gone, so a finished capture cannot be sent twice.
  void release(DebugCaptureState endState);

  DebugCaptureState state() const { return (DebugCaptureState)m_state; }
  void setState(DebugCaptureState s) { m_state = s; }

  // Samples captured so far. Reading this while the SpindleTask is recording
  // is a single aligned int load - it may be one sample stale, which is what a
  // progress readout wants.
  int count() const { return m_count; }
  int capacity() const { return m_capacity; }

  // The captured trace, for the uploader. Null when nothing is held.
  const DebugData* data() const { return m_buffer; }

  // True when there is a finished trace worth uploading. DBG_FAILED counts:
  // the buffer is deliberately kept after a failed POST so a retry does not
  // need the cut re-run.
  bool readyToSend() const {
    return m_buffer != 0 && m_count > 0 &&
           (m_state == DBG_FULL || m_state == DBG_FAILED);
  }

  // Allocator seam, for tests only (test/test_debugcapture drives the size
  // ladder and the DBG_NOMEM path through it). Defaults to malloc/free.
  static void setAllocatorForTest(void* (*alloc)(size_t), void (*dealloc)(void*));

 private:
  DebugData* volatile m_write;   // cursor; valid only while m_recording
  DebugData* m_buffer;           // base of the allocation, or null
  volatile bool m_recording;
  volatile int m_state;          // DebugCaptureState, as an aligned int
  volatile int m_count;
  int m_capacity;
  uint32_t m_startMicros;
  uint32_t m_lastSampleMicros;
  int m_lastDirection;
  // Per-iteration bookkeeping for the starvation pair. Touched only from the
  // SpindleTask, and only while recording.
  uint32_t m_lastIterationMicros;
  uint32_t m_peakGapUs;
  int m_peakDelta;
  int m_peakDeltaMagnitude;

  static void* (*s_alloc)(size_t);
  static void (*s_free)(void*);
};

// --- Wire format -----------------------------------------------------------
//
// CSV, not a binary blob. The trace is ~2400 rows; at ~90 bytes a row that is
// ~215 KB over WiFi, which costs nothing and buys: a sink that can write the
// body straight to a file with no decoder, no endianness or float-layout
// contract to document and get wrong, and a capture the owner can open in a
// spreadsheet when the analysis script says something surprising. It is also
// generated a row at a time into a small stack buffer, so the upload needs no
// second large allocation next to the trace it is sending.

// The header line (no trailing newline), unchanged from the serial dump.
const char* debugCsvHeader();

// --- Operator-visible status ------------------------------------------------
//
// The one line the Diagnostics screen shows in place of its title while a
// capture exists. Short by necessity - it shares a 14px row with the exit hint
// and goes through the display's 20-byte text cache, which SILENTLY TRUNCATES
// anything longer and then never compares equal again, repainting at 10 Hz
// forever (see the TextSlot note in the display header). Hence the length test
// in test/test_debugcapture.
//
// It lives here, next to the state enum it renders, so the states and their
// wording cannot drift apart, and so the length constraint is checkable by the
// host tests - lib/display is not built for the native env.
static const size_t kCaptureStatusMax = 20;  // including the terminator
void formatCaptureStatus(char* out, size_t n, int state, int count,
                         int capacity);

// Formats one sample as a CSV row WITHOUT a trailing newline. Returns the
// number of characters written (excluding the terminator), or a negative value
// if `n` was too small. kDebugCsvRowMax is a safe buffer size for any sample.
static const size_t kDebugCsvRowMax = 192;
int formatDebugRow(char* out, size_t n, const DebugData& d);

// --- URL parsing -----------------------------------------------------------
//
// Splits an http:// or https:// URL into the parts the raw-socket uploader
// needs. Lives here rather than in src/ so it is covered by the host tests -
// src/ is not built for the native env.
//
// Returns false for anything that is not a well-formed http/https URL with a
// non-empty host. `path` is "/" when the URL has none. `port` defaults to 80
// for http and 443 for https, or the explicit ":port" if given.
struct HttpUrlParts {
  bool secure;
  char host[128];
  int port;
  char path[256];
};
bool parseHttpUrl(const char* url, HttpUrlParts& out);

#endif  // ELS_DEBUGCAPTURE_H
