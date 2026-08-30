#include <config.h>
#ifdef ELS_USE_BUTTON_ARRAY
#include <keyarray.h>

// Deliberately no <globalstate.h> here any more. KeyArray is a pure input
// device: it reports key events and encoder detents and decides nothing. The
// encoder used to call GlobalState::next/prevFeedPitch() from
// updateEncoderPos(), which is how turning the knob inside a widget could step
// the pitch behind the operator's back; that decision now belongs to
// lib/ui/uistate.cpp, which is host-tested. Do not reintroduce the include.

// The Leadscrew is no longer needed either - setTargetPitchMM() went the same
// way as the GlobalState calls - but the parameter is kept so main.cpp's
// construction is untouched.

// KeyScanner reports events in its own enum so lib/keyscan stays free of the
// Arduino headers this file needs. The two must agree, and nothing at runtime
// would notice if they stopped.
static_assert((int)KS_NONE == (int)BS_NONE, "keyscan/ButtonState drift");
static_assert((int)KS_PRESSED == (int)BS_PRESSED, "keyscan/ButtonState drift");
static_assert((int)KS_CLICKED == (int)BS_CLICKED, "keyscan/ButtonState drift");
static_assert((int)KS_HELD == (int)BS_HELD, "keyscan/ButtonState drift");
static_assert((int)KS_RELEASED == (int)BS_RELEASED, "keyscan/ButtonState drift");

KeyArray::KeyArray(Leadscrew* leadscrew)
#ifdef ELS_UI_ENCODER
    // EncoderDetents has no default constructor on purpose - the counts per
    // detent is a property of the board (board.h), not a thing to forget.
    : m_detents(ELS_UI_ENCODER_COUNTS_PER_DETENT)
#endif
{
    (void)leadscrew;
    // EVERY member. KeyArray is heap-allocated (main.cpp `new KeyArray`), so
    // members are NOT zero-initialised the way the previous static instance
    // was. This is the class where that bit historically: an uninitialised
    // keycodeMillis made the old debounce permanently true and swallowed every
    // button press. m_scanner initialises itself (keyscan.cpp).
    readindex = 0;
    writeindex = 0;
    m_ringDrops = 0;
#ifdef ELS_UI_ENCODER
    // No internal pull-ups: GPIO34-39 are input-only and do not have any. A
    // and B are held up by R11/R12 (10K to +3.3V) on the board instead.
    ESP32Encoder::useInternalWeakPullResistors = puType::none;

    // FULL QUADRATURE, not single edge. Both edges of both channels, so a
    // contact bounce nets to zero in the counter instead of accumulating as
    // real motion - that is what stopped the knob skipping items and stepping
    // backwards. The full account is in lib/keyscan/encoderdetents.h; do not
    // revert this to attachSingleEdge to "save" the divide by four, which is
    // the only thing it costs.
    m_encoder.attachFullQuad(ELS_UI_ENCODER_A, ELS_UI_ENCODER_B);

    // 1023 APB cycles - 12.79 us at 80 MHz, and the hardware maximum, which
    // the library clamps to. Too short to touch mechanical bounce by two to
    // three orders of magnitude (see encoderdetents.h); kept because it is
    // exactly right for EMI and for the ~80 ns GPIO36/39 SAR-ADC glitch.
    m_encoder.setFilter(1023);

    m_detents.reset(m_encoder.getCount());
#endif
}

// Put the matrix into its resting configuration: columns driven high, rows read
// through a pull-down. getCodeFromArray() leaves it this way on every path, so
// each scan starts from a known state instead of inheriting whatever the last
// one left behind.
//
// NOTE what is absent: attachInterrupt(). Nothing is armed for an edge any
// more, which is what makes a stranded arming state impossible rather than
// merely unlikely (docs/keypad-audit.md §1).
void KeyArray::setupKeys() {
    pinMode(ELS_PAD_H1, INPUT_PULLDOWN);
    pinMode(ELS_PAD_H2, INPUT_PULLDOWN);
    pinMode(ELS_PAD_H3, INPUT_PULLDOWN);

    pinMode(ELS_PAD_V1, OUTPUT);
    pinMode(ELS_PAD_V2, OUTPUT);
    pinMode(ELS_PAD_V3, OUTPUT);
    digitalWrite(ELS_PAD_V1, 1);
    digitalWrite(ELS_PAD_V2, 1);
    digitalWrite(ELS_PAD_V3, 1);
}

// The scan task. Its own task rather than a slot in the DisplayTask because the
// display runs at 100 ms and a 100 ms sampling period would make the debounce
// meaningless and every press feel late.
//
// Core 0, with the display and the network stacks - core 1 belongs to the
// spindle loop alone (main.cpp). Priority 2: above the DisplayTask so a long
// LVGL repaint cannot stretch the sampling interval, far below the WiFi driver
// at 23. It sleeps between samples, so it costs a few GPIO reads per 2 ms.
static void KeyScanTask(void* parameter) {
    (void)parameter;
    for (;;) {
        keyArray->poll();
        vTaskDelay(KeyArray::kKeyScanPeriodMs / portTICK_PERIOD_MS);
    }
}

void KeyArray::initPad() {
    setupKeys();
    // 3 KB is ample: poll() has no recursion, no printf and one small array of
    // events on the stack.
    xTaskCreatePinnedToCore(KeyScanTask, "KeyScan", 3072, nullptr, 2, nullptr, 0);
}

ButtonInfo KeyArray::consumeButton() {
    if (readindex == writeindex)
        return { 0, ButtonState::BS_NONE };
    ButtonInfo ret = ringBuffer[readindex];
    readindex = (readindex + 1) % kRingSize;
    return ret;
}

void KeyArray::emitButton(int code, int state) {
    const int next = (writeindex + 1) % kRingSize;
    if (next == readindex) {
        // FULL, and reported as such. The old buffer had no such test: a
        // writeindex that wrapped onto readindex made the ring read as EMPTY,
        // so an overflow did not drop the newest event - it silently dropped
        // every event in the buffer. Here the queued gestures survive and the
        // loss is counted; ringDrops() should never leave zero.
        m_ringDrops++;
        return;
    }
    ringBuffer[writeindex].button = code;
    ringBuffer[writeindex].buttonState = state;
    writeindex = next;
}

void KeyArray::poll() {
    KeyScanOut out[kKeyScanMaxEvents];
    const int n = m_scanner.update(getCodeFromArray(), millis(), out,
                                   kKeyScanMaxEvents);
    for (int i = 0; i < n; i++) {
        emitButton(out[i].code, out[i].event);
    }
}

int KeyArray::consumeEncoderDelta() {
#ifdef ELS_UI_ENCODER
    // Sample the hardware here rather than in consumeButton(): the encoder is
    // not part of the key ring buffer, and ButtonPad drains that buffer in a
    // loop but asks for detents exactly once per pass.
    //
    // Everything else - the divide by the counts per detent, the sub-detent
    // residue, the glitch drop and the bound ButtonPad's replay loop relies on
    // - is in EncoderDetents, where the host tests can reach it.
    return m_detents.update(m_encoder.getCount());
#else
    return 0;
#endif
}

// One complete matrix read, in task context.
//
// Two phases: drive the columns and read the rows to find WHICH row is active,
// then drive that row alone and read the columns to find which column. The
// combination is the key code (see the table in lib/config/board.h).
//
// Both phases end by restoring the resting configuration, so the next call is
// independent of this one. The short settle delays are new: switching a pin
// from OUTPUT to INPUT_PULLDOWN leaves the line to discharge through the
// pull-down, and reading before it has settled is a misread. The old code did
// this inside an ISR where a delay was unthinkable; in a task it costs nothing
// worth counting.
int KeyArray::getCodeFromArray() {
    // Phase 1: columns driven high (the resting state), read the rows.
    const int a = digitalRead(ELS_PAD_H1) | (digitalRead(ELS_PAD_H2) << 1) |
                  (digitalRead(ELS_PAD_H3) << 2);

    // Phase 2: flip. Rows become outputs, and only the row that read high is
    // driven, so the column read below cannot be confused by a second key in a
    // different row.
    pinMode(ELS_PAD_V1, INPUT_PULLDOWN);
    pinMode(ELS_PAD_V2, INPUT_PULLDOWN);
    pinMode(ELS_PAD_V3, INPUT_PULLDOWN);
    pinMode(ELS_PAD_H1, OUTPUT);
    pinMode(ELS_PAD_H2, OUTPUT);
    pinMode(ELS_PAD_H3, OUTPUT);
    digitalWrite(ELS_PAD_H1, a == 1 ? 1 : 0);
    digitalWrite(ELS_PAD_H2, a == 2 ? 1 : 0);
    digitalWrite(ELS_PAD_H3, a == 4 ? 1 : 0);
    delayMicroseconds(5);

    const int b = digitalRead(ELS_PAD_V1) | (digitalRead(ELS_PAD_V2) << 1) |
                  (digitalRead(ELS_PAD_V3) << 2);

    // Anything that is not exactly one row and one column reads as "nothing".
    // Two keys at once land here (a or b has two bits set, so neither matches
    // the single-bit tests above and the driven row is none of them), which is
    // the honest answer for a matrix that cannot resolve them - and KeyScanner
    // treats it as a release rather than a phantom key.
    const int code = (a == 0 || b == 0) ? 0 : a | b << 3;

    setupKeys();
    delayMicroseconds(5);
    return code;
}
#endif
