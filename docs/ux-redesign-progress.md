# UX redesign — implementation progress

Working log for the `ux-redesign` branch build-out. Written for Martin to read on return.

## Baselines (before any change)

| | Value |
|---|---|
| Host tests | 54 cases, all passing |
| Device build | SUCCESS |
| Flash | 1,506,049 / 2,031,616 (74.1%) — ~525 KB headroom |
| RAM | 114,896 / 327,680 (35.1%) |

## Method

Per feature set: **test-author → implementer → reviewer**, each a separate agent, each its own
commit. Models matched to role — Haiku for mechanical work, Sonnet for standard
implementation, Opus for review and for design-heavy logic. Feature sets run in parallel only
where they touch disjoint files.

Verification is done by me between stages (`pio test -e native`, `pio run -e esp32dev_usb`),
not taken from agent self-reports.

### A structural constraint that shaped the decomposition

PlatformIO's test runner builds `lib/` but **not** `src/`. That is why the ESP-only files in
`src/` don't break the native env — and it means anything that needs host tests must live in
`lib/`. So the focus state machine and the DRO datum logic became new pure-C++ libraries
(`lib/ui/`, `lib/dro/`) rather than living in `src/buttonpad.cpp` where they would have been
untestable.

## Feature sets

| ID | Scope | Files | Status |
|----|-------|-------|--------|
| A | GlobalState feed/jog semantics | `lib/global_state`, `lib/config/config.h` | tests + impl done; review outstanding |
| B | Carriage + stop position in mm | `lib/leadscrew` | **complete** — all three stages |
| C | DRO datum resolution | `lib/dro` (new) | **complete** — all three stages |
| D | UI focus state machine | `lib/ui` (new) | **complete** — all three stages |
| D2 | `motionActive` gating + run reconciliation | `lib/ui` | tests done; impl running |
| E | Real sync action | `lib/leadscrew` | not started |
| F | LatheConfig fields + save API + CHECKVALUE | `lib/config`, `src/WebSettings.*` | tests done; impl running |
| G | board.h remap + buttonpad rewrite + lock removal | `src/`, `lib/global_state` | not started |
| H | Icons I1 → A8 | `lib/display/icons` | impl + review done |
| H2 | Restore icon colour parity | `lib/display` | running |
| I | Theme palette + display rebuild | `lib/display` | not started |
| J | RTC boot-to-setup flag | `src/main.cpp` | not started |

### A sequencing mistake worth recording

The design doc's own dependency order (§9) put **theme support before the A8 icon conversion**.
I ran them the other way round, and that opened a real window: LVGL's A8 draw branch is
*unconditional* — unlike every other format it does not gate on `recolor_opa`, it always applies
`draw_dsc->recolor`, whose style default is **black**. The eight 32×32 status icons had *white*
ink baked into their I1 palettes and sit on coloured state rectangles, so they went black-on-green
for the pervasive "engaged" state. Caught in review, fixed by FS-H2 restoring explicit recolour.
The lesson is that the doc's ordering was load-bearing, not decorative.

## Decisions taken while you were away

These were judgement calls made to keep moving. All are cheap to reverse — say the word.

1. **`FM_JOG` becomes unreachable, not deleted.** Removing it from the mode cycle leaves no
   public way to reach it (there is no `setFeedMode()`), so the `FM_JOG` branch inside
   `next/prevFeedPitch()` becomes provably dead code and goes. The enumerator itself stays,
   because `lib/display/` and `src/` still reference it and removing it breaks the device
   build. One test that reached FM_JOG by cycling was deleted as unsatisfiable, and replaced
   with one covering the half of the requirement that survives.
2. **`kMenuItemCount = 9`** was added to the `UiState` API — clamping the menu index is
   impossible without a tile count. Nine matches the tiles in spec §6.
3. **HALT fires on `Press` and `Hold`, not just `Click`.** A safety key should act at the
   earliest possible event, and `CancelMotion` is idempotent so a double-fire is harmless.
4. **Icons converted by faithful 1-bit expansion**, so they are A8 but not anti-aliased. True
   smooth edges need regenerating from the SVGs in `assets/` — deliberately not attempted,
   since it would change the artwork rather than just the format.

## A pre-existing bug worth fixing — imperial feed is 1000× too slow

Found by the FS-A review, confirmed independently, **not fixed** — it is outside this
redesign's scope and it changes how the machine actually cuts, so it is your call.

`feedPitchImperial[]` (`lib/config/config.h`) is commented `// defined as thou/rev` but holds
`0.002 … 0.030`. Those are **inches** per rev — 2 to 30 thou, which are the real feed rates for
a lathe this size. The comment is wrong, and two pieces of code disagree about it:

| | Treats the table as | At index 8 (`0.010`) |
|---|---|---|
| `Display::drawPitch()` | inches — renders `* 1000` | shows `10th` ✓ |
| `GlobalState::getCurrentFeedPitch()` | thou — computes `* 25.4 / 1000` | commands **0.000254 mm/rev** ✗ |

They cannot both be right, and the display is. The correct conversion is `* 25.4`, giving
0.254 mm/rev. As it stands, **imperial feed mode feeds at one thousandth of the rate shown on
screen** — the screen says 10 thou and the carriage creeps.

Metric feed and both threading modes are unaffected (they use different tables and paths), and
imperial *threading* is fine — it goes through the TPI branch. It is imperial FEED only, which
is presumably why it has survived.

The fix is one line plus a test asserting the correct value. I have not applied it because it
changes machine motion and belongs in its own commit, not buried in a UI branch.

## Safety findings from review — read this bit first

The FS-D review turned up two genuine hazards in the focus state machine. Both are fixed or
in progress, but they are the most important thing that happened overnight.

**1. Editing endstops while the machine is moving.** `hitLeftEndstop()` is the *sole* arrest
for an engaged feed, and `unsetStop()` sets the stop position to `INT32_MIN` — so clearing a
stop mid-cut does not move the arrest, it deletes it permanently and the carriage feeds into
the chuck. Clearing the *far* stop is quieter but re-extrapolates the helix anchor through an
integer truncation, invisible until the next pass cuts in the wrong place. Setting is no safer:
the display task polls at 10 Hz, so at 1250 rpm × 1.25 mm/rev the captured position is up to
~2.6 mm from where the operator actually pressed — and a stop in the wrong place is worse than
no stop, because it gets trusted on the next pass.

Stop edits are now inert whenever the carriage is under power. The STOPS key still takes focus,
because the travel bar is exactly what you want on screen mid-cut; only the edits are blocked.

**2. The dead-man jog release was conditional.** Three separate early returns in the jog branch
each swallowed the arrow `Release` and left the carriage running with no `JogStop`. A
hold-to-jog is only safe if letting go *always* stops it, so the terminator is now
unconditional and sits directly under HALT. It is deliberately not direction-matched — erring
toward "stop" costs a shortened jog; erring the other way costs a crash.

**3. Pre-existing hazard, NOT introduced here, and not yet fixed.** `src/keyarray.cpp` can drop
a `Release` entirely: `handleRelease()` returns early inside a 10 ms debounce, so a very short
tap emits `Press` with no `Release` at all; and `handleTimer()` emits a release for button `0`
rather than the arrow if any second key is touched during a hold. A dead-man jog that never
receives its release cannot stop itself. Today's `FM_JOG` path has the identical hazard, so
this is not a regression — but a dead-man switch whose release can go missing is only a
dead-man switch by luck. **This is the one I would want your eyes on**, and it is the reason
FS-G ships with a `// HAZARD:` comment rather than a fix.

Two candidate fixes, both behaviour changes I did not want to make unilaterally:

- **Time-cap a continuous jog** — auto-decelerate after N ms with no `Release`. Simple,
  bounded, no new coupling. Costs you a jog that stops itself mid-traverse if you pick N too
  short, and a long deliberate traverse is a legitimate thing to want.
- **Poll the matrix from the display loop** while a jog is in flight, and stop the moment the
  key is no longer physically down. This is a true dead-man rather than an approximation, but
  `getCodeFromArray()` has side effects — it reconfigures pin modes and re-attaches the
  interrupts — so calling it from the display task races the ISR. It would need that scan
  split into a side-effect-free read first.

I lean to the second done properly, because the first only shortens the window rather than
closing it. But it is a real change to how the keypad is read, on the path that moves the
carriage, so it is your call.

## Known regressions on this branch

Tracked, not accidental. All are consequences of removing a key before its replacement exists.

1. **OTA has no on-device trigger.** The half-nut hold that started an update is gone with the
   key, and its designed replacement is the Software update menu tile, which does not exist
   until the display rebuild. **Do not flash this branch to the lathe expecting to update it
   over the air afterwards** — you would be back to USB until the menu lands. If you want a
   temporary binding in the meantime (MENU-hold would be the obvious one) it is a two-line
   change; I did not add it unasked because it is a UI decision and it would need removing
   again.
2. **Units toggle is unreachable.** MODE-hold used to switch metric/imperial; that moves to
   the Units tile, so MODE-hold is currently inert.
3. **Display reset is unreachable.** Thread-sync-hold used to call `setDisplayReset()`; that
   key no longer exists.
4. **The lock is bypassed but not removed.** `GlobalButtonLock`, `getButtonLock()` and
   `drawLocked()` all still exist because `lib/display` calls them — deleting them now would
   break the device build. The new buttonpad simply never consults the lock, and the
   `ButtonPad` constructor unlocks once at startup so the padlock does not sit on screen and
   so the encoder is not silently swallowed (`keyarray.cpp:102`). All of it dies with the
   display rebuild.

## A landmine for whoever wires up ZeroDro

`OK`-hold emits `ZeroDro`, and it is deliberately allowed while the machine is engaged — it
moves no metal, and the DRO is a display concern. That reasoning only holds while the zero is
a **display datum**.

If it is ever implemented as `Leadscrew::setCurrentPosition(0)`, it stops being a display
concern: the endstops are stored as absolute positions against the same counter, so rezeroing
mid-cut would silently shift both stops relative to the tool. The datum must live beside the
DRO (a stored `manualZeroPulses`, exactly as `lib/dro` already models it) and must never move
the carriage's own position counter.

`Leadscrew::setCurrentPosition()` exists and has no non-test callers. It is the obvious wrong
answer, which is why it is written down here.

## Questions for Martin

_(accumulated as they arise; nothing here blocks the work below it)_

1. **"Hold STOPS to clear both stops" has no home yet.** The spec (§4) calls for it with a 1 s
   confirm bar, but there is no `ClearBothStops` intent in the `UiIntent` enum, so it is
   currently unimplemented. The test author flagged it rather than inventing one, which was
   the right call. Do you want it? It needs an enum addition plus a confirm-bar interaction
   that nothing else in the design uses — worth confirming it earns that complexity before I
   build it.
2. **Anti-aliased icons.** Worth regenerating the twelve assets from `assets/*.svg` at A8 for
   genuinely smooth edges? It is a visual improvement the current conversion cannot give, but
   it changes the artwork rather than just the encoding, so I have not done it unasked.
3. **The menu has no vocabulary for editing a tile in place.** Spec §6 says the DRO datum tile
   is "◀ ▶ pick Left / Right" and jog speed is "◀ ▶ adjust inline", but while the menu is open
   the arrows map unconditionally to `MenuPrev`/`MenuNext` — there is no notion of an
   "activated" tile. A two-value toggle could be faked by making `OK` cycle it, but the
   six-step jog speed cannot. Either the menu model grows an editing sub-state, or those two
   spec lines change to "OK opens a sub-widget". I lean to the sub-state, but it is a design
   decision, not an implementation one, so it is waiting for you.
4. **Should a run-to-stop be cancellable by the opposite arrow, or only the same one?** §3 says
   "a second press of the same arrow"; §7 says "cancellable by any of three keys". The
   implementation takes the broader, safer reading — either arrow cancels. Flagging only
   because the two spec lines disagree.

## Infrastructure notes found the hard way

- **Parallel `pio` runs collide.** They share `.pio` and fight over the scons DB lock
  (`.sconsign311.dblite`), which produces spurious `ERRORED` results in suites nobody touched.
  Fix: every agent gets its own `PLATFORMIO_BUILD_DIR` under the scratchpad. If you ever see
  inexplicable `ERRORED` output from a parallel run, this is why — it is not a code fault.
- **Windows `pio test` reports `ERRORED` / `CTRL_BREAK_EVENT` for any non-zero googletest
  exit**, i.e. for any failing test. Cosmetic wrapper behaviour. Judge pass/fail from
  googletest's own printed summary, never from the PlatformIO suite status.

## Log

- Baseline captured; 54/54 host tests green, device build clean.
- Wave 1 test authors launched for A, B, C, D (disjoint files, run in parallel).
- FS-B tests committed (12 cases). Contract set: `getStopPositionMM()` returns `NAN` when
  unset; the pulse sentinels are never converted. Implementer running.
- FS-C tests committed (24 cases). Four implicit rules surfaced and folded back into
  `docs/ux-redesign.md` — stop-at-zero, stale pulse fields, coincident stops, and whose job
  the sentinel translation is. Implementer running.
