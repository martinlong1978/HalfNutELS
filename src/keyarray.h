#ifdef ELS_USE_BUTTON_ARRAY
#ifndef KEYARRAY_H
#define KEYARRAY_H

#include <Arduino.h>
#include <leadscrew.h>
#include <spindle.h>
#include <ESP32Encoder.h>

void buttonInterrupt();
void buttonInterruptRelease();
void IRAM_ATTR timerInterrupt();

enum ButtonState { BS_NONE = 0, BS_PRESSED = 1, BS_CLICKED = 2, BS_HELD = 3, BS_RELEASED = 4, BS_DOUBLE_CLICKED = 5 };

typedef struct buttonInfo {
    int button;
    int buttonState;
} ButtonInfo;

class KeyArray {
private:
    volatile ButtonInfo buttonState;
    volatile unsigned long keycodeMillis;
    ButtonInfo ringBuffer[10];
    volatile int readindex = 0;
    volatile int writeindex = 0;
    hw_timer_t* Timer0_Cfg;

    void setupKeys(bool press);
    int getCodeFromArray();
    void emitButton();
#ifdef ELS_UI_ENCODER
    ESP32Encoder m_encoder;
    int64_t encoderPos;
    // Detents seen since the last consumeEncoderDelta(), signed: + clockwise.
    // Accumulated here and nowhere else - see consumeEncoderDelta().
    int m_encoderDetents;
#endif
public:
    KeyArray();
    void initPad();
    void handle();
    void handleRelease();
    void handleTimer();
    ButtonInfo consumeButton();

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

    KeyArray(Leadscrew* leadscrew);
};

extern KeyArray *keyArray;
#endif
#endif