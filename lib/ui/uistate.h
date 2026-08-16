// UI focus state machine for the Mk2 button panel (docs/ux-redesign.md §1-§6).
//
// Pure C++ on purpose: NO Arduino / ESP / FreeRTOS includes, so it builds and is
// unit-tested on the native host (`pio test -e native`). This is why the focus
// logic does NOT live in src/buttonpad.cpp.
//
// The object is a pure decision function: it owns focus + menu state and turns
// (key, event, context) into a single UiIntent for the caller to execute. It
// never touches GlobalState, the leadscrew or the display.
//
// Event vocabulary matches KeyArray (src/keyarray.cpp:144-170):
//   short press -> Press, Click, Release
//   long  press -> Press, Hold,  Release   (no Click after a Hold)
// The state machine relies on that ordering; see the notes on each handler.
#ifndef ELS_UI_UISTATE_H
#define ELS_UI_UISTATE_H

// The field the arrows currently drive. Rests on Jog and returns there.
enum class UiFocus { Jog, JogSpeed, Rate, Mode, Stops, Menu };

// The eight physical keys of the Mk2 panel (ENABLE is handled outside the focus
// model - it is machine state, not a focus target, so it has no entry here
// beyond the panel itself).
enum class UiKey { Mode, Rate, Stops, Left, Ok, Right, Halt, Menu };

enum class UiKeyEvent { Press, Release, Click, Hold };

// What the caller should do. Exactly one per key event; None means "nothing".
enum class UiIntent {
  None,
  JogLeftStart, JogRightStart, JogStop,
  RunToLeftStop, RunToRightStop,
  CancelMotion,
  PitchNext, PitchPrev,
  JogSpeedNext, JogSpeedPrev,
  ModeNext, ModePrev,
  SetLeftStop, ClearLeftStop,
  SetRightStop, ClearRightStop,
  ZeroDro,
  MenuNext, MenuPrev, MenuActivate,
  CloseMenu,
};

// Machine state the decision depends on. Supplied fresh by the caller on every
// key event; UiState never caches it.
struct UiContext {
  bool leftStopSet;
  bool rightStopSet;
  bool motionEnabled;   // true when the leadscrew is engaged (MM_ENABLED)
};

class UiState {
 public:
  UiState();

  UiFocus focus() const;
  bool menuOpen() const;
  int  menuIndex() const;

  // Feed one key event. Returns the single action the caller should perform.
  // Any key event resets the focus idle timeout.
  UiIntent handleKey(UiKey key, UiKeyEvent ev, const UiContext& ctx,
                     unsigned long nowMs);

  // Call periodically from the display task. Returns true if the focus timeout
  // fired and focus changed, so the caller can use it as a redraw trigger.
  bool tick(unsigned long nowMs);

  // Focus falls back to Jog after this long with no key events (§1). Menu focus
  // is exempt - the menu leaves only on MENU or HALT.
  static const unsigned long kFocusTimeoutMs = 4000;

  // Number of menu tiles (docs/ux-redesign.md §6: Units, Theme, DRO datum, Jog
  // speed, Sync, Software update, Setup/Wi-Fi, Diagnostics, About). menuIndex()
  // is clamped to [0, kMenuItemCount - 1]; it does NOT wrap.
  static const int kMenuItemCount = 9;

 private:
  UiFocus m_focus;
  bool m_menuOpen;
  int m_menuIndex;
  unsigned long m_lastActivityMs;
  // Direction of an in-flight powered run to a stop: -1 left, +1 right, 0 none.
  // A second arrow click while this is non-zero cancels the run.
  int m_runToStopDir;
  // Direction of an in-flight hold-to-jog: -1 left, +1 right, 0 none.
  int m_jogDir;
};

#endif  // ELS_UI_UISTATE_H
