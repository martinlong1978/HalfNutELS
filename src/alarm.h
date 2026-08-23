#ifndef ELS_SRC_ALARM_H
#define ELS_SRC_ALARM_H

// The stepper driver's alarm: the hardware half of lib/alarm/alarmmonitor.h.
//
// This file owns the two GPIOs (ELS_STEPPER_ALARM in, ELS_STEPPER_ENA out - see
// the polarity notes in lib/config/board.h), the task that samples them, and
// the publication of the result onto GlobalState. Every DECISION - debounce,
// latch, when to pulse, whether a clear worked - is in AlarmMonitor, which is
// pure C++ and host-tested (test/test_alarm). Nothing here decides anything.

// Configure the pins and start the alarm task. Call from setup(), and call it
// ABOVE the SpindleTask creation like everything else there (see the realtime
// note in main.cpp - nothing after that line ever runs).
//
// This REPLACES the bare pinMode/digitalWrite of ELS_STEPPER_ENA that used to
// sit in setup(): the enable line now has exactly one writer, the alarm task,
// and a second one would fight the reset pulse.
void alarmInit();

// The operator pressed OK on the alarm modal (UiIntent::ClearAlarm). Asks for
// the ENA reset pulse; the alarm task decides whether it worked. Safe to call
// from the DisplayTask - it sets one volatile flag.
void alarmRequestClear();

#endif
