#include "alarm.h"

#include <Arduino.h>
#include <alarmmonitor.h>
#include <globalstate.h>

#include "board.h"

// The stepper-alarm task. See alarm.h for the division of labour and
// lib/config/board.h for both pin polarities.
//
// WHERE IT RUNS. Core 0, priority 2 - alongside KeyScan, and for the same
// reasons: core 1 belongs to the spindle loop alone (CLAUDE.md), and a poll
// this fast has no business in the 100 ms DisplayTask. Priority 2 puts it above
// the display so a long LVGL repaint cannot stretch the sampling interval, and
// far below the WiFi driver at 23.
//
// Its own task rather than a few lines inside KeyScanTask, even though the two
// have the same shape and cadence: KeyArray is deliberately a pure input device
// that decides nothing and touches no GlobalState (see the note at the top of
// src/keyarray.cpp), and this stops the machine.
//
// WHY IT POLLS AND DOES NOT USE AN INTERRUPT. The same reasoning that took the
// keypad off edge interrupts (docs/keypad-audit.md): an edge scheme has an
// armed state that can be stranded, and a stranded alarm interrupt is a fault
// that never reaches the screen. A level, sampled every few milliseconds and
// integrated, cannot be missed - every sample re-reads the whole truth.
//
// The 5 ms period against AlarmMonitor::kDebounceMs (25 ms) gives five samples
// per decision, so a single dirty reading can never move the state machine.
// Worst case from the driver asserting to the axis being stopped is therefore
// ~30 ms - two orders of magnitude inside the 100 ms the panel would have taken
// to notice, and the reason the stop is issued from here rather than from
// ButtonPad's next pass.
static const unsigned long kAlarmPeriodMs = 5;

// The decision function. File-scope, single-threaded apart from the volatile
// clear-request flag its own header documents (alarmmonitor.h): only the task
// below calls update(), and only ButtonPad calls requestClear().
static AlarmMonitor alarmMonitor;

void alarmRequestClear() { alarmMonitor.requestClear(); }

// GlobalAlarmState is the wire format for AlarmState - the display and ButtonPad
// read the enum off GlobalState rather than reaching into the monitor, so the
// object stays owned by this task. One switch, here, so the mapping exists once.
static GlobalAlarmState publish(AlarmState s) {
  switch (s) {
    case AlarmState::Alarm: return AS_ALARM;
    case AlarmState::Clearing: return AS_CLEARING;
    case AlarmState::Clear:
    default: return AS_CLEAR;
  }
}

static void AlarmTask(void* parameter) {
  (void)parameter;
  GlobalState* globalState = GlobalState::getInstance();
  for (;;) {
    const bool asserted =
        (digitalRead(ELS_STEPPER_ALARM) == ELS_STEPPER_ALARM_ACTIVE_LEVEL);
    alarmMonitor.update(asserted, millis());

    // The enable line, driven unconditionally from the monitor's output rather
    // than only on a change. It is one register write, and it means no path
    // through this loop can leave the drivers switched off - the failure mode
    // that would look, from the operator's chair, like a dead lathe.
    digitalWrite(ELS_STEPPER_ENA, alarmMonitor.enaAsserted() ? 1 : 0);

    if (alarmMonitor.consumeTrip()) {
      // Once per trip: the helix anchor is worthless the moment the driver
      // stops honouring steps, so the sync state goes with it and the operator
      // is told (by the modal) that it has. Not held, unlike the motion mode
      // below - Leadscrew::update() re-asserts SS_UNSYNC on its own once the
      // axis settles at MM_DISABLED, and nothing can raise SS_SYNC while the
      // modal has the panel locked.
      globalState->setThreadSyncState(GlobalThreadSyncState::SS_UNSYNC);
    }

    if (alarmMonitor.active()) {
      // HELD, not written once on the trip edge. The panel does not find out
      // about the alarm until its next 100 ms pass, and in that window the
      // operator could still press ENABLE - so a single write on the edge
      // leaves a window in which the machine can be commanded to move with the
      // fault already latched. Holding it closes that window, and it also
      // covers the other commanders the panel does not speak for: the web UI,
      // and a spindle-driven feed that would otherwise resume by itself.
      //
      // MM_DISABLED and not MM_DECELLERATE, which is what HALT asks for: the
      // motor has already stopped, at the driver, so there is no ramp left to
      // run - and MM_DISABLED is what Leadscrew::update()'s own endstop arrest
      // publishes for exactly the same situation (leadscrew.cpp).
      //
      // Compared first so the common case - no alarm, or an alarm the axis has
      // already settled out of - costs a read and not a write.
      if (globalState->getMotionMode() != GlobalMotionMode::MM_DISABLED) {
        globalState->setMotionMode(GlobalMotionMode::MM_DISABLED);
      }
    }

    globalState->setAlarmState(publish(alarmMonitor.state()),
                               alarmMonitor.inputAsserted(),
                               alarmMonitor.clearFailed());

    vTaskDelay(kAlarmPeriodMs / portTICK_PERIOD_MS);
  }
}

void alarmInit() {
  // No internal pull: the board carries a 10K to +3.3V on this net (board.h),
  // and the ESP32's own ~45K would only fight it.
  pinMode(ELS_STEPPER_ALARM, INPUT);

  // The enable line starts LOW - drivers enabled - which is what setup() used
  // to write directly. From here on this task is its only writer.
  pinMode(ELS_STEPPER_ENA, OUTPUT);
  digitalWrite(ELS_STEPPER_ENA, 0);

  // 2 KB is ample: the loop has no recursion, no printf and no locals beyond a
  // pointer and two bools.
  xTaskCreatePinnedToCore(AlarmTask, "Alarm", 2048, nullptr, 2, nullptr, 0);
}
