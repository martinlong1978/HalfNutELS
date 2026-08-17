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

#endif  // ELS_UI_UISTATE_H
