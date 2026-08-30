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
  Diagnostics, About,                     // read-only screens: no expiry
  Alarm,                                  // the stepper-alarm modal. NOT chosen
                                          // by the operator - see below.
  // The OTA screen (GlobalState::hasOTA()). FORCED, on the ClearAlarm/
  // UiFocus::Alarm pattern, for exactly as long as ctx.ota is true - see the
  // note there. DECLARED HERE BY THE TEST AUTHOR (test/test_uistate/
  // test_uistate.cpp, "UiStateOta") as the interface the new tests compile
  // against; uistate.cpp does not yet branch on it, which is deliberate - the
  // gating behaviour is the implementer's job, and the failing UiStateOta
  // cases are the spec for it. See that test file for the full design
  // rationale (why this is a UiFocus rather than routed in ButtonPad, why
  // HALT is inert, why the whole hasOTA() span is gated and not only a
  // settled failure).
  Ota
};

// UiFocus::Alarm is the one focus nothing on the panel can ask for. The stepper
// driver has raised a fault (lib/alarm/alarmmonitor.h), motion has been stopped
// underneath the operator, and the panel's only remaining job is to say so and
// take the acknowledgement - so the focus is FORCED, from both handleKey() and
// tick(), for as long as ctx.alarm is true, and released the moment it is not.
//
// It is therefore neither a widget nor a read-only screen: it has no idle
// timeout (a modal that times out is a modal the operator can miss entirely),
// it cannot be dismissed by MENU or HALT, and it does not survive motion
// because it is the reason there is none. Every other key is inert while it is
// up, INCLUDING HALT - the one exception to "HALT is checked before anything"
// in the whole of this file, and it is only an exception in the letter: HALT
// exists to stop the machine, the machine is already stopped and held stopped,
// so there is nothing for it to do that the alarm has not already done.
//
// The lockout being total is the point. The operator has a crash to clear, and
// a panel that would still open a menu or step a pitch behind the dialog is a
// panel that can be operated in a state where nothing it reports is true - the
// carriage position and the thread sync are both stale the instant the driver
// stops stepping.

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
// obeys focus, the idle timeout and the motion lockout like everything else.
// src/buttonpad.cpp delivers exactly one Click per detent (no Press/Release -
// a detent is instantaneous, there is nothing to hold), and every other event
// on these two keys is inert.
//
// THE MOTION LOCKOUT (OWNER RULING) cuts across every key in this enum, so it
// is stated here once rather than key by key. While the carriage is under power
// - motionEnabled OR motionActive, i.e. underPower() in uistate.cpp - the panel
// answers only the gestures that STOP things:
//
//   Halt                 unchanged. Unconditional, from every focus.
//   Enable               unchanged. Dismiss-then-toggle, exactly as at rest.
//   Left / Right         ONLY their stopping halves: the Release that ends an
//                        in-flight hold-to-jog, and the Click/Hold that cancels
//                        a powered run to a stop. They may not start motion,
//                        move focus, or step any setting.
//   everything else      INERT. Mode, Rate, Stops, Menu, Ok and both encoder
//                        keys do nothing at all: no focus change, no widget, no
//                        carousel, no setting, no ZeroDro.
//
// The owner's words: "every button except halt and enable should be disabled
// whilst moving... When moving, all of the operator's attention should be on
// the tool and workpiece, not the screen/menus." The arrows are the one place
// that needed refining, because disabling them wholesale would delete the
// dead-man terminator and the run-cancel - the two gestures whose entire job is
// to stop the machine.
//
// This replaces five separate "moving, X disabled" rules with one, and it is
// deliberately BROADER than the guards it subsumes rather than a rewiring of
// them: menuTileBlock()'s motion arm, the Stops focus gate, the DroDatum arrow
// gate and the clear-both confirm re-check all remain in place below it, on
// paths the panel can no longer reach, because each of them is judged against
// a context the panel is not the only source of (the web UI and a spindle-
// driven feed can both move the carriage) and because a safety gate that is
// merely unreachable is not the same thing as one that is wrong.
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
  // TEST-AUTHOR DECLARATION (issue #11, round 2 - not yet implemented by
  // uistate.cpp). Hold on a side with a stop SET must emit ONE of these,
  // distinct from RunToLeftStop/RunToRightStop (which stay the Click-run
  // intent), so the caller (src/buttonpad.cpp) can route it to a NEW
  // GlobalMotionMode that arrests at the stop like MM_JOG_* AND runs at
  // jogSpeedPps() * GlobalState::getJogSpeed() like MM_INTERACTIVE_JOG_* -
  // see lib/global_state/globalstate.h's MM_HOLD_JOG_LEFT/RIGHT, added
  // alongside this for the same reason. Bookkeeping (m_jogDir vs m_runPhase)
  // is unchanged from the prior round: this still records in m_jogDir so the
  // dead-man terminator ends it on Release with JogStop.
  JogToLeftStop, JogToRightStop,
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
  // OK on the stepper-alarm modal: pulse the driver's ENABLE line to reset it
  // (AlarmMonitor::requestClear()). NOT "dismiss the dialog" - the dialog goes
  // when the alarm does, and if the fault is still present at the driver the
  // clear fails and the modal stays up saying so.
  ClearAlarm,
  // OK on the OTA screen: acknowledges the outcome (OtaOutcome::acknowledge(),
  // safe to call at any phase - see its header comment). Declared alongside
  // UiFocus::Ota and UiContext::ota for the same reason - see the note there.
  AckOta,
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
  bool alarm;          // the stepper driver has raised a LATCHED fault, or is
                       // in the middle of the reset pulse that clears one -
                       // i.e. GlobalState::alarmActive(). Both states inhibit
                       // the machine and both keep the modal up, so they are
                       // one flag here; the display tells them apart from
                       // GlobalState because only it needs to.
                       //
                       // Read BEFORE anything else in handleKey() and tick(),
                       // above even HALT. See the ruling on UiFocus::Alarm.
  bool ota;            // GlobalState::hasOTA() - the OTA screen owns the whole
                       // display for as long as this is true, exactly as the
                       // alarm modal does, and for the same reason nothing on
                       // the panel may act invisibly behind it: forces
                       // UiFocus::Ota (see the enum). True for the WHOLE
                       // attempt (connecting/checking/downloading/settled),
                       // not only a settled failure, because the display
                       // shows nothing else for any of those phases either.
                       // See test/test_uistate/test_uistate.cpp, "UiStateOta",
                       // for the full rationale; uistate.cpp does not act on
                       // this yet (test-author scaffolding, see UiFocus::Ota).
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

  // Call periodically from the display task, with a context sampled from the
  // machine exactly as handleKey()'s is. Returns true if focus CHANGED, so the
  // caller can use it as a redraw trigger.
  //
  // Two things can change focus here, and the return contract is the same for
  // both: true only on the transition, false on every later call that finds
  // nothing left to do.
  //   * the 4 s idle timeout (§1), which applies to the widget focuses only;
  //   * CLOSE-ON-MOTION (OWNER RULING): the instant the carriage is under
  //     power, any open widget, read-only screen or carousel is closed and
  //     focus returns to Jog.
  //
  // Why the context is a parameter rather than something UiState remembers from
  // the last key event: motion very often starts with no key event at all - the
  // web UI, a spindle-driven feed, the natural end of a run - and reconciling
  // on the next handleKey() would leave a picker on screen over a moving
  // carriage for as long as the operator did not touch the panel, which is
  // exactly the state the ruling exists to forbid. This is the only call that
  // happens unconditionally, every display pass, so it is the only place the
  // close can be guaranteed. The cost is one extra argument at the single call
  // site (src/buttonpad.cpp), which already builds a context each pass.
  bool tick(const UiContext& ctx, unsigned long nowMs);

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
  // speed, Sync, Software update, Setup/Wi-Fi, Diagnostics, About) plus Debug
  // capture, which §6 does not describe because it is a diagnostic instrument
  // rather than a setting. menuIndex() is clamped to [0, kMenuItemCount - 1];
  // it does NOT wrap.
  static const int kMenuItemCount = 10;

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

  // SELECTOR TOGGLE, the STOPS half (OWNER RULING: "menu opens the menu,
  // pressing a second time closes it. The same logic should apply to rate, mode
  // and stops").
  //
  // MODE and RATE need no state for this - they take focus on the Click, so a
  // Click that finds its own focus already current is unambiguously a SECOND
  // press and closes. STOPS cannot use that test alone, because it takes focus
  // on the PRESS (see the long note in uistate.cpp): from Jog a single tap is
  // Press (which opens the widget) and then Click, and by the time that Click
  // arrives the focus is already Stops, so the naive rule would close what the
  // Press of the SAME gesture had just opened.
  //
  // So this records "the press that is physically down right now is the one that
  // opened the widget", and the Click declines to close while it is set. It is
  // set on every STOPS Press - true when that press moved focus INTO Stops,
  // false when the widget was already open - and cleared on the Release, so it
  // describes one press and never outlives it.
  //
  // The close deliberately hangs off the CLICK and not the Press, which is what
  // keeps clear-both alive: KeyArray emits no Click after a Hold
  // (src/keyarray.cpp:144-170), so press-and-hold on an already-open widget
  // never reaches the closing branch and still fires ClearBothStops.
  //
  // Cleared anywhere focus can be forced out from under a live press, so it can
  // never go stale: any event on any other key (top of handleKey - which is what
  // covers HALT and the ENABLE dismiss, both of which return through there), the
  // motion lockout, and tick()'s close-on-motion.
  bool m_stopsOpenedByPress;
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
  // Arms / discards a motion-trace capture (lib/global_state/debugcapture.h).
  // APPENDED AT THE END, and it must stay there: the carousel is index-driven
  // and so is every test that walks it, so inserting a tile anywhere else
  // silently renumbers the ones after it - the "tile 5 fires tile 6, which
  // REBOOTS" failure the note above this enum warns about.
  MENU_DEBUG_CAPTURE,
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
  // Debug capture - arming calls malloc for a ~100 KB trace buffer, and
  // discarding frees it. Neither belongs under a running cut, and the tile is
  // useless there anyway: a capture has to be ARMED BEFORE the cut starts, or
  // it will not contain the cut. Blocking it here is what makes "arm it, then
  // engage" the only possible order.
  case MENU_DEBUG_CAPTURE:
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
  // Debug capture lands on Diagnostics, which is where the capture's state is
  // displayed - armed / how full / waiting to send / sent. Same rule as every
  // other tile: OK closes the menu and leaves you looking at the result.
  case MENU_DEBUG_CAPTURE: return UiFocus::Diagnostics;
  default:               return UiFocus::Jog;
  }
}

#endif  // ELS_UI_UISTATE_H
