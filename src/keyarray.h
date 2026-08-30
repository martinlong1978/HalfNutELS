#ifdef ELS_USE_BUTTON_ARRAY
#ifndef KEYARRAY_H
#define KEYARRAY_H

#include <Arduino.h>
#include <keyscan.h>
#include <leadscrew.h>
#include <spindle.h>
#include <ESP32Encoder.h>
#include <encoderdetents.h>

enum ButtonState { BS_NONE = 0, BS_PRESSED = 1, BS_CLICKED = 2, BS_HELD = 3, BS_RELEASED = 4, BS_DOUBLE_CLICKED = 5 };

typedef struct buttonInfo {
    int button;
    int buttonState;
} ButtonInfo;

// The 3x3 keypad matrix and the rotary encoder.
//
// POLLED, NOT INTERRUPT-DRIVEN, and that is the whole point of the current
// design - see docs/keypad-audit.md for what it replaced and why. The short
// version: the old scheme armed a RISING interrupt for a press and a FALLING
// one for a release, re-armed as a side effect of each scan, behind a 10 ms
// lockout shared between the two. A release inside that window returned early
// BEFORE the re-arming line, leaving the pad waiting for a FALLING edge on a
// line that was already low - so the keypad went dead until the 1 s hold timer
// happened to rescan. It also called pinMode() and attachInterrupt() from
// inside the ISR, from flash, without IRAM_ATTR.
//
// None of that exists now. A small task scans the matrix every
// kKeyScanPeriodMs and hands the raw code to KeyScanner (lib/keyscan), which
// does the debouncing and gesture recognition in pure, host-tested C++. There
// are no interrupts, nothing to arm, and every sample reads the whole matrix -
// so no reading can leave the pad unable to see the next one.
//
// This class therefore owns only the hardware: drive the columns, read the
// rows, and queue whatever KeyScanner returns for ButtonPad to drain.
class KeyArray {
private:
    // Ring buffer between the scan task (producer) and the DisplayTask
    // (consumer, via ButtonPad::handle()). Single producer, single consumer,
    // and both indices are 32-bit aligned - atomic on the ESP32 - so no lock
    // (CLAUDE.md, cross-task state).
    //
    // 32 slots, not 10. A gesture is at most three events and they only occur
    // on transitions, so at a 2 ms scan and a 100 ms drain this cannot fill in
    // practice; the size is chosen so that it cannot fill in theory either.
    ButtonInfo ringBuffer[32];
    volatile int readindex;
    volatile int writeindex;

    // Events discarded because the ring was full. Should stay at zero; it is
    // here because the OLD buffer could not distinguish full from empty (a
    // wrapped writeindex read as "empty" and silently lost all ten events), and
    // a counter is how that class of fault stops being invisible.
    volatile unsigned long m_ringDrops;

    KeyScanner m_scanner;

    void setupKeys();
    int getCodeFromArray();
    void emitButton(int code, int state);
#ifdef ELS_UI_ENCODER
    ESP32Encoder m_encoder;
    // Raw counts -> detents. All of the arithmetic lives in this pure-C++
    // object (lib/keyscan/encoderdetents.h) so it is host-tested; this class
    // keeps only the PCNT unit it reads from.
    EncoderDetents m_detents;
#endif
public:
    // Ring capacity. Public because ButtonPad bounds its drain loop on it - a
    // drain shorter than the buffer leaves events waiting a whole display pass.
    static const int kRingSize = 32;

    // How often the matrix is sampled. Fast enough that press latency is
    // imperceptible, cheap enough to be free - a scan is a handful of GPIO
    // operations. kKeyDebounceMs (lib/keyscan) is expressed in milliseconds so
    // the two stay independent.
    static const unsigned long kKeyScanPeriodMs = 2;

    void initPad();

    // One matrix sample plus whatever gesture it completes. Called from the
    // scan task only.
    void poll();

    ButtonInfo consumeButton();

    // Diagnostics for docs/keypad-audit.md §6: readings rejected as bounce, and
    // events lost to a full ring. Both should be judged from the machine rather
    // than assumed - the first says whether kKeyDebounceMs suits this hardware.
    unsigned long bounceRejects() const { return m_scanner.bounceRejects(); }
    unsigned long ringDrops() const { return m_ringDrops; }

    // Detents accumulated since the last call, then reset to zero. Positive is
    // clockwise.
    //
    // This is the WHOLE of the encoder's effect on the machine. It used to act
    // directly: updateEncoderPos() called GlobalState::next/prevFeedPitch() and
    // Leadscrew::setTargetPitchMM() itself, so the knob reached past the focus
    // model entirely and stepped the pitch even while a widget or the menu was
    // open. That was masked by the old button lock, which swallowed every
    // detent; the Mk2 panel unlocks at boot, so it became live from power-on.
    // KeyArray now touches neither GlobalState nor the Leadscrew - it reports
    // motion, and ButtonPad feeds it to UiState as UiKey::EncoderCw/Ccw so the
    // knob goes through exactly the same path as every key.
    int consumeEncoderDelta();

#ifdef ELS_UI_ENCODER
    // Counter artefacts discarded by the detent decoder, alongside
    // bounceRejects()/ringDrops(). Should stay at zero.
    unsigned long encoderGlitchDrops() const { return m_detents.glitchDrops(); }
#endif

    KeyArray(Leadscrew* leadscrew);
};

extern KeyArray *keyArray;
#endif
#endif
