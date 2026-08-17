#ifdef ELS_USE_BUTTON_ARRAY
#ifndef ELS_BUTTONPAD_H
#define ELS_BUTTONPAD_H

#include <latheconfig.h>
#include <leadscrew.h>
#include <spindle.h>
#include <keyarray.h>
#include <uistate.h>

/**
 * Glue between the physical keypad (KeyArray) and the UI focus state machine
 * (lib/ui/uistate.h), per docs/ux-redesign.md Sec. 1-6.
 *
 * Deliberately thin: this class translates matrix codes and ButtonStates into
 * (UiKey, UiKeyEvent), asks UiState what to do, and executes the single
 * returned UiIntent. Every decision - what focus is, what an arrow means, when
 * a stop may be edited - lives in UiState, which is pure C++ and covered by
 * host tests (`pio test -e native`). Nothing in src/ is host-testable, so no
 * new decision logic belongs here.
 *
 * Runs on the DisplayTask (core 1, priority 1, 100 ms loop - main.cpp), never
 * on the spindle hot loop.
 */
class ButtonPad {
 private:
  Spindle *m_spindle;
  Leadscrew *m_leadscrew;
  KeyArray *m_pad;

  // Focus / menu state machine. Value member: it owns no resources, allocates
  // nothing, and its constructor initialises all of its own fields.
  UiState m_ui;

  // Working copy of the persisted lathe settings, so the Theme and DRO datum
  // menu tiles have something to hand saveLatheSettings() (WebSettings.h).
  //
  // A COPY, and unavoidably so. The live LatheConfig is a local in main.cpp's
  // setup(); the only handle on it anywhere else is LatheConfigDerived, which
  // keeps its LatheConfig* private and exposes read-only accessors. And
  // getLatheSettings() is not an alternative: it heap-allocates a FRESH struct
  // read straight out of flash on every call, so writing through that would
  // both leak and diverge from the running configuration.
  //
  // Seeded in the constructor from LatheConfigDerived's accessors, which mirror
  // every LatheConfig member one-for-one (lib/config/latheconfig.cpp), so this
  // copy starts byte-identical to the live struct. The two tiles then mutate
  // and persist it. Because it is a copy, a save that is REFUSED must roll the
  // field back - otherwise the copy silently disagrees with flash and the next
  // successful save would write a value the user never chose.
  LatheConfig m_settings;

  // Matrix code (ELS_*_BUTTON, lib/config/board.h) -> UiKey. Returns false for
  // 0 / unknown codes and for ENABLE, which is not part of the focus model.
  static bool codeToKey(int code, UiKey &key);

  // KeyArray ButtonState -> UiKeyEvent. Returns false for states UiState has no
  // vocabulary for (BS_NONE, BS_DOUBLE_CLICKED - the latter is never emitted).
  static bool stateToEvent(int buttonState, UiKeyEvent &ev);

  // Machine state UiState needs to decide. Sampled fresh on every key event.
  UiContext buildContext();

  void applyIntent(UiIntent intent);

  // Runs the tile UiState::menuIndex() currently names (docs/ux-redesign.md
  // Sec. 6). The index -> tile mapping is NOT defined here: it is the MenuTile
  // enum in lib/ui/uistate.h, shared with the carousel that renders it.
  void activateMenuTile();

  // ENABLE sits outside the focus model (it is machine state, not a focus
  // target), so it is handled here rather than by UiState.
  void enableHandler(ButtonInfo press);

 public:
  ButtonPad(Spindle *spindle, Leadscrew *leadscrew, KeyArray *pad);

  void handle();

  // Read by the display so it can render the focus outline / open widget.
  const UiState &ui() const { return m_ui; }
};
#endif
#endif
