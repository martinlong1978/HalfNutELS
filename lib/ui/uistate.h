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
//
// Three groups, and the grouping is the whole of the idle-timeout rule (see
// isWidgetFocus() in uistate.cpp and tick() below):
//
//   Jog                        the rest state - nothing to fall back to.
//   JogSpeed/Rate/Mode/Stops/  the SELECTOR WIDGETS. Small pickers over the
//   DroDatum                   rest screen; they commit on OK and expire back
//                              to Jog after kFocusTimeoutMs.
//   Menu                       the carousel. Exempt: it leaves on MENU or HALT.
//   Diagnostics/About          READ-ONLY SCREENS. Also exempt - see below.
//
// DroDatum is a widget because it is one: two choices, arrows pick, OK commits,
// and leaving it open over the rest screen with the arrows re-pointed at a
// setting is exactly the hazard the 4 s timeout exists for.
//
// Diagnostics and About are NOT, and that is a deliberate ruling rather than an
// oversight. Three reasons:
//   * The timeout protects against a PICKER being left open, where the next
//     arrow press would silently change a setting the operator has forgotten is
//     focused. A read-only screen has no such hazard: its arrows are inert
//     (see the arrow switch in uistate.cpp), so there is nothing to guard.
//   * Diagnostics exists to be WATCHED. Following error and pulse counts only
//     mean anything over a spindle revolution or a test pass, which is tens of
//     seconds, not four - and the operator's hands are on the machine, not the
//     panel, so there is no input to keep the timer alive with. A screen that
//     vanishes mid-observation, needing MENU plus seven arrow presses to get
//     back, is worse than no screen.
//   * About is a screen you read something OFF - a version string, an IP
//     address you are typing into a phone. Same shape, same answer, and one
//     rule for both read-only screens is easier to hold than two.
// Both leave on OK, MENU or HALT: three keys, all of which an operator already
// reaches for to get out of something, which is the same bargain UiFocus::Menu
// already makes for its own exemption.
enum class UiFocus {
  Jog,
  JogSpeed, Rate, Mode, Stops, DroDatum,  // the widgets: OK commits, 4 s expiry
  Menu,
  Diagnostics, About                      // read-only screens: no expiry
};

// The nine physical keys of the Mk2 panel, plus the two synthetic keys the
// rotary encoder produces.
//
// ENABLE is IN the focus model, not beside it. It used to be handled in
// src/buttonpad.cpp ahead of this translation, but §5's "toggle MM_ENABLED" is
// only half of what the key does: engaging is a commitment to cut, and it must
// not happen while the operator's attention is still inside a picker. That
// "first press dismisses, second engages" decision is exactly the kind of thing
// that has to be host-testable, so it lives here and ButtonPad only executes
// the resulting UiIntent::ToggleEngage.
//
// EncoderCw / EncoderCcw are one detent of the rotary encoder, clockwise and
// anticlockwise. They are keys and not a separate API on purpose: the encoder
// used to reach past the focus model entirely and drive the pitch directly out
// of src/keyarray.cpp, so turning it inside any widget silently stepped the
// pitch behind the operator's back. Routing it through handleKey() means it
// obeys focus, the idle timeout and the motion inhibit like everything else -
// and that inhibit is a blanket one: while the carriage is under power the knob
// does nothing in ANY focus (see the encoder branch in uistate.cpp).
// src/buttonpad.cpp delivers exactly one Click per detent (no Press/Release -
// a detent is instantaneous, there is nothing to hold), and every other event
// on these two keys is inert.
enum class UiKey {
  Mode, Rate, Stops, Left, Ok, Right, Halt, Menu, Enable,
  EncoderCw, EncoderCcw
};

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
  ClearBothStops,
  ZeroDro,
  // The DRO datum picker (UiFocus::DroDatum). ABSOLUTE, not a next/prev pair,
  // and not a toggle - see the long note on the DroDatum arrow branch in
  // uistate.cpp for why. The caller persists and applies the named end.
  DroDatumLeft, DroDatumRight,
  MenuNext, MenuPrev, MenuActivate,
  CloseMenu,
  ToggleEngage,
};

// Machine state the decision depends on. Supplied fresh by the caller on every
// key event; UiState never caches it.
//
// "Fresh" is load-bearing for motionActive, not a figure of speech. The powered
// run latch (m_runPhase, below) is reconciled against it, and the reconciliation
// can only observe a run ending if some key event, at some point, saw the run
// while it was live. The caller must therefore EXECUTE the returned intent and
// then rebuild the context from the machine before the next handleKey - not
// snapshot one context per display poll and feed it to a whole batch of queued
// key events. See the m_runPhase note below for what goes stale otherwise.
struct UiContext {
  bool leftStopSet;
  bool rightStopSet;
  bool motionEnabled;   // true when the leadscrew is engaged (MM_ENABLED)
  bool motionActive;   // the carriage is under power right now:
                       // motionMode is neither MM_DISABLED nor MM_UNSET.
                       // A superset of motionEnabled: it also covers the
                       // powered run to a stop (MM_JOG_*), the interactive jog
                       // (MM_INTERACTIVE_JOG_*) and the deceleration tail
                       // (MM_DECELLERATE).
  bool threadMode;     // the feed mode is FM_THREAD or FM_THREAD_REVERSE.
                       // Needed here for exactly one decision: OK on a menu
                       // tile now CLOSES the carousel and moves focus, so
                       // UiState has to know whether the tile would be refused
                       // before it can decide where to leave the operator - and
                       // menuTileBlock() (below) refuses Sync outside a thread
                       // mode. It is the SAME menuTileBlock() the display dims
                       // with and ButtonPad re-checks against fresh GlobalState,
                       // not a second copy of the rule.
};

class UiState {
 public:
  UiState();

  UiFocus focus() const;
  bool menuOpen() const;

  // The selected tile. CONTRACT: this stays valid after MenuActivate has closed
  // the carousel, and the caller depends on it - ButtonPad reads it to decide
  // WHICH tile to execute, after handleKey() has already returned. The index is
  // reset only when the menu is OPENED (every open starts at 0), never when it
  // closes. Do not "tidy" that by clearing it on close: the tile that fires
  // would silently become tile 0, i.e. Units instead of whatever was selected.
  int  menuIndex() const;

  // Feed one key event. Returns the single action the caller should perform.
  // Any key event resets the focus idle timeout.
  UiIntent handleKey(UiKey key, UiKeyEvent ev, const UiContext& ctx,
                     unsigned long nowMs);

  // Call periodically from the display task. Returns true if the focus timeout
  // fired and focus changed, so the caller can use it as a redraw trigger.
  bool tick(unsigned long nowMs);

  // How far through the "hold STOPS to clear BOTH stops" gesture we are, in
  // permille: 0 at the instant of the press, 1000 when the hold fires (§4,
  // "STOPS hold - clear both, after a 1 s confirm bar"). 0 whenever the gesture
  // is not running.
  //
  // OWNER: the display feature set (lib/display) - it is the only caller. This
  // class provides the number and renders nothing; drawing the bar over the
  // STOPS widget belongs to whoever owns that overlay.
  //
  // Why a permille poll and not an "armed" flag plus a start time: the display
  // redraws on its own 100 ms cadence with no key event to hang off, so it needs
  // to be able to ask "how full is the bar NOW" at an arbitrary moment. Handing
  // out the raw press timestamp would export a member that only means anything
  // in combination with the gesture's own preconditions (focus, motion state,
  // whether any stop exists at all), and the display would then have to re-derive
  // those - a second copy of the rule, which is exactly the drift §4 and the
  // menuTileBlock() note below both warn about. An integer fraction keeps the
  // decision here: a non-zero return means "this hold is live and WILL clear
  // both stops if it completes", so the bar can never fill for a gesture the
  // machine is going to refuse.
  //
  // NOT a timer. The gesture's one second is KeyArray's existing hold timer
  // (src/keyarray.cpp:58, `timerAlarmWrite(Timer0_Cfg, 1000000, true)`), which
  // is what delivers the Hold that actually fires the intent. This is only the
  // fill fraction for the same second, so the two cannot disagree about when
  // the hold completes - see kStopsConfirmMs.
  int stopsConfirmPermille(unsigned long nowMs) const;

  // Focus falls back to Jog after this long with no key events (§1). Applies to
  // the widget focuses only. Menu is exempt (it leaves on MENU or HALT), and so
  // are Diagnostics and About - see the ruling on the UiFocus enum above.
  static const unsigned long kFocusTimeoutMs = 4000;

  // The length of the clear-both confirm bar. This MIRRORS KeyArray's hold
  // timer (src/keyarray.cpp:58) - it does not define the gesture, it only says
  // how fast the bar fills so that it reaches full exactly as the Hold arrives.
  // If that timer ever changes, change this with it.
  static const unsigned long kStopsConfirmMs = 1000;

  // Number of menu tiles (docs/ux-redesign.md §6: Units, Theme, DRO datum, Jog
  // speed, Sync, Software update, Setup/Wi-Fi, Diagnostics, About). menuIndex()
  // is clamped to [0, kMenuItemCount - 1]; it does NOT wrap.
  static const int kMenuItemCount = 9;

 private:
  // How far a powered run to a stop has got. Two phases, not one flag, because
  // the caller cannot report the run instantly: it is COMMANDED from the moment
  // the intent is emitted, and CONFIRMED once some later context has actually
  // reported motionActive. Only a CONFIRMED run may be inferred to have ENDED
  // when a context reports motionActive false - a COMMANDED one may simply not
  // have started yet, and an inactive context says nothing about it. See the
  // long note in uistate.cpp for why both halves are necessary.
  //
  // No direction is stored. Every consumer asks only "is a run in flight" - the
  // cancel is unconditional on EITHER arrow, by design (§7), and the intent that
  // starts a run is chosen from the key that starts it, not from this member.
  enum class RunPhase { None, Commanded, Confirmed };

  UiFocus m_focus;
  bool m_menuOpen;
  int m_menuIndex;
  unsigned long m_lastActivityMs;
  RunPhase m_runPhase;
  // Direction of an in-flight hold-to-jog: -1 left, +1 right, 0 none.
  int m_jogDir;

  // The clear-both confirm gesture (§4). m_stopsConfirming is true only while a
  // STOPS press is physically down AND that press could still succeed - it is
  // set on the Press, which is also where STOPS takes focus, when the carriage
  // is at rest and there is at least one stop to clear. (With the menu open the
  // press never reaches that code at all, so the bar cannot arm from there.)
  // m_stopsPressMs is the timestamp of that Press and is meaningless while
  // m_stopsConfirming is false. Both are cleared by the Release, by the Hold
  // that consumes them, by HALT, and by any event on any other key.
  bool m_stopsConfirming;
  unsigned long m_stopsPressMs;
};

// --- The menu tiles (docs/ux-redesign.md section 6, "MENU") ----------------
//
// This lives beside UiState, not in lib/display, because menuTileBlock() below
// is safety-relevant motion-gating logic (it decides whether Sync, Software
// update, Wi-Fi setup, Theme and DRO datum are allowed to fire) and has to be
// covered by the native host tests (`pio test -e native`), which cannot build
// anything behind <lvgl.h>. UiState itself still knows nothing about what the
// tiles ARE - it owns only the interaction (menuOpen/menuIndex/kMenuItemCount,
// the saturating clamp) - but the tile IDENTITIES and the availability RULE
// share this header so they can never drift out of step with UiState's count.
//
// Two consumers, and they must never disagree about what an index means:
//   * lib/display/ST7789_320_240displaylvgl.cpp renders the carousel from it
//     (Display::drawOverlayMenu() / drawOverlay());
//   * src/buttonpad.cpp dispatches UiIntent::MenuActivate on it
//     (ButtonPad::activateMenuTile()).
// A second, private copy of the order on either side is exactly how tile 5
// ("Software update") ends up firing tile 6 ("Wi-Fi setup", which REBOOTS).
// The static_assert below is what ties it back to UiState's count - now
// trivially checkable, since both live in the same header.
enum MenuTile {
  MENU_UNITS = 0,
  MENU_THEME,
  MENU_DRO_DATUM,
  MENU_JOG_SPEED,
  MENU_SYNC,
  MENU_SOFTWARE_UPDATE,
  MENU_WIFI_SETUP,
  MENU_DIAGNOSTICS,
  MENU_ABOUT,
  MENU_TILE_COUNT
};
static_assert((int)MENU_TILE_COUNT == UiState::kMenuItemCount,
              "menu tile list and UiState::kMenuItemCount disagree - the "
              "carousel would render or dispatch a tile the other side has "
              "never heard of");

// Why the selected tile cannot be activated right now, or MTB_NONE.
//
// Same rule, ONE evaluation, both sides: the display dims the tile and puts the
// reason in the hint row, and ButtonPad refuses the activation. Splitting them
// is how a menu ends up offering a gesture the machine will silently ignore -
// the mistake docs/ux-redesign.md section 4 calls out for the STOPS hint.
enum MenuTileBlock {
  MTB_NONE,       // the tile is live
  MTB_MOTION,     // the carriage is under power - stop it first
  MTB_FEED_MODE,  // Sync means nothing outside a thread mode
};

// `motionActive` is the SAME predicate UiContext::motionActive carries:
// motionMode is neither MM_DISABLED nor MM_UNSET (so it covers the engaged
// feed, the powered run to a stop, the interactive jog AND the deceleration
// tail). `threadMode` is FM_THREAD or FM_THREAD_REVERSE.
//
// Why each tile blocks on motion:
//   Theme / DRO datum   - both persist through saveLatheSettings(), which
//                         REFUSES while under power (src/WebSettings.h): a
//                         flash erase disables the instruction cache on both
//                         cores and stalls the spindle loop for tens of ms.
//                         Rendering them live while the write cannot happen
//                         would be the silent failure that guard exists to
//                         prevent, so they dim with the rest.
//   Sync                - setSyncPoint() zeroes the following error and raises
//                         SS_SYNC, which releases update()'s re-sync gate. Do
//                         that while the gate is holding the axis and the
//                         carriage lurches up to a full pitch (measured
//                         0.32 mm) into the work, and a residual lost-update
//                         race on m_expectedPosition is reachable only in that
//                         state. Section 6 words this as "against a stopped
//                         spindle"; the enforceable form is the AXIS being
//                         disengaged, since the re-sync gate can only be
//                         holding while the leadscrew is under power.
//   Software update     - hands the CPU to a TLS download for a minute.
//   Wi-Fi setup         - reboots.
// Units, Jog speed, Diagnostics and About touch neither flash nor motion, so
// they stay live throughout.
inline MenuTileBlock menuTileBlock(int tile, bool motionActive,
                                   bool threadMode) {
  switch (tile) {
  case MENU_SYNC:
    if (motionActive) {
      return MTB_MOTION;
    }
    return threadMode ? MTB_NONE : MTB_FEED_MODE;
  case MENU_THEME:
  case MENU_DRO_DATUM:
  case MENU_SOFTWARE_UPDATE:
  case MENU_WIFI_SETUP:
    return motionActive ? MTB_MOTION : MTB_NONE;
  default:
    return MTB_NONE;
  }
}

// Where OK on a tile leaves the operator (OWNER RULING, this feature set).
//
// The rule it encodes, in one line: **OK always closes the menu, and you always
// land somewhere that shows the result.** The menu used to leave itself open
// over a change the operator could not see - "OK does nothing visible", "no
// feedback that it worked" - because the carousel is a full-width panel sitting
// on top of the pitch, the theme and the travel bar, i.e. on top of the three
// things the tiles change. The confirmation is not a message; it is the screen
// you land on:
//
//   Units / Theme / Sync / Software update / Wi-Fi setup -> Jog
//       The main screen IS the confirmation. The pitch redraws in the new unit,
//       the whole screen changes colour, the sync indicator lights in the status
//       bar. Software update lands on Jog too and the OTA screen takes over from
//       GlobalState::hasOTA() - it is not a UiFocus, so there is nothing to name
//       here. Wi-Fi setup reboots, so its destination is academic; Jog is the
//       honest answer for the microseconds before it goes.
//   DRO datum -> DroDatum, Jog speed -> JogSpeed
//       Their effect is a SETTING, not something visible on the rest screen at
//       the moment of the press, so each opens its own picker instead.
//   Diagnostics / About -> their own read-only screens.
//
// THE ONE EXCEPTION is a refused tile (menuTileBlock() != MTB_NONE): it changes
// nothing and the carousel stays open with the reason already in the hint row.
// That is decided in UiState::handleKey(), not here - this function answers only
// "where does this tile go", and is never consulted for a tile that is blocked.
//
// Shared with lib/display for the same reason MenuTile and menuTileBlock() are:
// the screen that renders a destination and the state machine that moves to it
// must not hold two copies of the mapping.
inline UiFocus menuTileDestination(int tile) {
  switch (tile) {
  case MENU_DRO_DATUM:   return UiFocus::DroDatum;
  case MENU_JOG_SPEED:   return UiFocus::JogSpeed;
  case MENU_DIAGNOSTICS: return UiFocus::Diagnostics;
  case MENU_ABOUT:       return UiFocus::About;
  default:               return UiFocus::Jog;
  }
}

#endif  // ELS_UI_UISTATE_H
