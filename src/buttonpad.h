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

  // NOTE: there is deliberately NO LatheConfig member here. The Theme and DRO
  // datum tiles used to mutate a working copy of the whole struct and hand it
  // to saveLatheSettings(), which wrote all of it - so a theme toggle rewrote
  // the user's commissioned geometry out of that copy. Geometry is now web-only
  // and carried through flash by saveLathePreferences() (src/WebSettings.h);
  // the tiles read the current preferences with readLathePreferences() at the
  // moment of the press and pass two values. Nothing to cache, and nothing to
  // roll back when a save is refused.

  // Matrix code (ELS_*_BUTTON, lib/config/board.h) -> UiKey. Returns false for
  // 0 / unknown codes. ENABLE maps like every other key now: UiState decides
  // whether it dismisses an open widget or toggles the leadscrew.
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
  //
  // SIDE EFFECTS ONLY. UiState has already closed the carousel and moved focus
  // to menuTileDestination(tile) by the time this runs - "OK always closes the
  // menu, and you always land somewhere that shows the result" - so several
  // tiles now legitimately do nothing here. Do not move focus from this method.
  void activateMenuTile();

  // Persist + apply one end of the travel as the DRO datum, behind
  // UiIntent::DroDatumLeft / DroDatumRight. Idempotent: a request for the end
  // already stored writes nothing, which is what keeps a repeated press off the
  // flash erase path. DroDatumPreference comes from lib/dro/dro.h via
  // latheconfig.h above.
  void setDroDatumPreference(DroDatumPreference wanted);

  // The MM_ENABLED <-> MM_DECELLERATE swap behind UiIntent::ToggleEngage. Pure
  // execution: whether ENABLE toggles at all, or merely dismisses whatever is
  // on screen, is decided in UiState so it can be host-tested.
  void enableHandler();

 public:
  ButtonPad(Spindle *spindle, Leadscrew *leadscrew, KeyArray *pad);

  void handle();

  // Read by the display so it can render the focus outline / open widget.
  const UiState &ui() const { return m_ui; }
};
#endif
#endif
