# UX redesign — implementation progress

Working log for the `ux-redesign` branch build-out. Written for Martin to read on return.

## State as of the second flash

Everything from the first-hardware feedback is done, and there is now a screen renderer, so
visual claims can be checked instead of argued.

| | |
|---|---|
| Host tests | 354, 12 suites |
| Device build | clean — Flash ~75.9%, RAM 35.1% |
| Screens rendered | 38, in `tools/screenshot/out/` |

**Done since the first flash:** the real palette (dark is now actually dark); the pitch slider
replaced by discrete pips, which is what the mockup always specified and is honest about a
twenty-value list; bands re-spaced; state chip back to 26; soft-key hints removed; the menu
carousel three-across as the mockup; every menu tile given a destination with `OK` closing the
carousel; `DroDatum`, `Diagnostics` and `About` screens built; the Wi-Fi screens brought onto the
palette; the sync **anchor source** surfaced for the first time.

**Known imperfect, deliberately:**
- Light-theme green stop ticks are 2.24:1 on white — below the 3:1 component guideline. Fixing it
  properly means diverging the two palettes' state hues, which §8 says to share.
- Light-theme accent fill vs surface is 1.84:1; the selected tile leans on hue plus dark ink.
- Setup screen: the gap between credential *rows* is airier than label-to-value.
- `diagnostics-error` shows `NOT SYNCED` beside `ANCHOR L stop` — honest (an anchor exists, sync is
  lost) but a reader might expect the anchor to dim.
- `getEstimatedVelocityInPulsesPerSecond()` returns 0 below ~1000 pps, so Diagnostics will read
  `CARRIAGE 0.00` on very slow fine feeds. Firmware behaviour, not display — but it will look like
  a fault on the bench.

**Still not done:** the clear-both confirm bar does not render (the widget opens, but no filling
bar); `GlobalButtonLock` still exists unused; and the imperial-feed 1000× bug is still there by
your decision.

## Read this first

The branch builds clean and every host test passes. Nine of the twelve feature sets are
complete through all three agent stages. **Nothing has been run on hardware** — everything here
is host tests plus a clean device compile, and the parts that touch flash and RTC memory
cannot be tested off-device at all.

Three things want your decision before this goes near the lathe, in order:

1. **Imperial feed is 1000× too slow.** Pre-existing, confirmed, deliberately not fixed — it
   changes how the machine cuts and belongs in its own commit. One line plus a test. Details
   in the section below.
2. **The keypad can drop a key release**, so a dead-man jog can fail to stop itself.
   Pre-existing and not a regression, but the redesign leans on hold-to-jog much harder than
   the old firmware did. Two candidate fixes written up; neither applied.
3. **OTA has no on-device trigger on this branch.** The half-nut hold went with the key and the
   menu that replaces it does not exist yet.

The most valuable thing the reviewers found that I had missed: **a flash erase stalls the
spindle loop on both cores**, so saving a setting mid-cut would lose spindle sync. That one is
fixed — `saveLatheSettings()` now refuses while the carriage is under power.

If you only read one more section, make it "Safety findings from review".

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

## Post-flash findings — first time on real hardware

The branch was flashed and looked at. Three things came back.

### 1. It is very grey — and that is a self-inflicted wound

The vivid palette in §8 has **never actually been used**. When the theme system went in, the
acceptance criterion was that dark mode be *byte-identical to the then-current screen*, so
`PALETTE_DARK` kept the old washed-out values (`0x008800` green, `0xCCCCCC` grey) and `init()`
deliberately did not paint a background — leaving LVGL's default light grey `0xF5F5F5`. On glass:
pale grey ground, black text, dim chips.

That constraint was right for a safe refactor and then never lifted. **The step that applies the
actual design was never scheduled.** It is now.

### 2. The soft-key hints go

`HALT / MENU / RUN` along the bottom duplicated what the physical caps will say. Removed; the
space goes to the state readout and possibly the travel band.

### 3. The menu felt disconnected — because most tiles have no destination

Owner's diagnosis: *OK does nothing visible*, and *no feedback that it worked*. Both true.
Diagnostics and About are no-ops, DRO datum has no overlay to open, and the tiles that DO fire
(Units, Theme, Sync) change something off-screen while the menu covers it.

**The rule, decided: `OK` always closes the menu, and you always land somewhere that shows the
result.** The main screen *is* the confirmation — units redraw, the theme changes, the travel bar
re-datums. Tiles whose effect is not visible there open their own screen instead. The single
exception is a refused tile, which stays open with its reason, as it already does.

That means building the missing destinations (Diagnostics, About, a DRO-datum overlay) and
changing `MenuActivate`, which currently leaves the carousel open.

## A screen renderer now exists (`tools/screenshot/`)

Every visual claim on this branch was reasoned from coordinates and font tables and never seen —
which is exactly how the grey shipped. LVGL is portable C, and the only reason it was excluded
from the host build is that `TFT_eSPI` and `Arduino.h` are not. With those shimmed, the real
`Display` renders into a buffer and writes PNGs.

Use it before believing anything about the screen.

## Decisions from the review session

Settled with Martin after the overnight run. These supersede anything below that disagrees.

### The governing principle

**Lathe geometry is web-only and must never be writable from the device.** Encoder PPR, stepper
PPR, gearbox ratio and leadscrew pitch are commissioning values — set once, over Wi-Fi, with the
lathe offline. The on-device menu covers only what you would genuinely tweak *during a session*.

This is not just policy, it fixes three logged defects at a stroke. `ButtonPad` currently
reconstructs all nine geometry fields from the derived config and rewrites every one of them to
flash on a theme toggle. Under the new rule the on-device save re-reads the stored config and
overwrites **only the two preference bytes**. Geometry is then never sourced from RAM, which
removes the `sizeof` padding blind spot, the two-sources-of-truth wart, and the
null-config-writes-defaults path together.

### Settled

| # | Decision |
|---|---|
| 1 | **Geometry web-only.** On-device save touches only preference bytes (above). |
| 2 | **Auto-return is rejected outright** — see below. Not deferred; it cannot work on this machine. |
| 3 | **Zero-on-set**, new preference: setting a stop zeroes the DRO there, but **only for the stop that matches the datum preference**. The preference stays in charge; this just saves a separate zeroing gesture. |
| 4 | **Encoder**: drives pitch at rest, acts as ◀/▶ inside a widget or menu, and is **inert in STOPS focus** — a knob is far easier to nudge than a key, and setting an endstop stays a deliberate keypress. |
| 5 | **Dead-man jog**: poll the matrix while a jog is in flight and stop the moment the key is no longer down. The real fix, not the time-cap approximation. |
| 6 | **Imperial feed 1000× bug: leave it.** Stays a documented known bug, not a to-do. Do not "tidy" it. |
| 7 | **Menu editing**: `OK` on a tile closes the menu and opens the matching overlay. No editing sub-state — one grammar, and it reuses the overlays that already exist. |
| 8 | **Stale leadscrew speed after arrest**: fix by decaying on the gated path. |
| 9 | **Clear-both-stops**: build it as specced, with the 1 s confirm bar. |
| 10 | **New antialiased mode glyphs**, and delete the nine dead assets (~16 KB of flash). |
| 11 | **ENABLE with a widget open**: the first press closes the widget *only*; a second press engages. You cannot commit to a cut while your attention is still in a picker. |
| 12 | **Diagnostics tile** shows live position error, spindle vs leadscrew rates, and sync anchor state. The old serial debug path stays dead. |

### Why auto-return is off the table

Worth recording because it is a machining fact, not a UI preference. Automatically running back
to the other stop after a pass **only works if there is a servo on the cross-slide to retract the
tool first**. Without one the return pass is still cutting, and it mars the thread you just made.
On this machine it would destroy work. Do not re-propose it.

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

## The Sync menu gate — investigated, NOT defeatable

**Resolved: the leak does not occur.** Peak following error after an arrest is always about one
revolution of feed and never more, at every speed and pitch tested, and it always settles to
exactly zero. Left- and right-hand threads mirror exactly. Carriage drift ≤ 3 pulses (0.01 mm)
over twenty revolutions.

But the reasoning on both sides was wrong in interesting ways, so it is worth recording rather
than deleting.

**The review's caveat was false.** It hinged on "if the deceleration planner always lands on
exactly zero speed there is no leak". It does not: 0 at 300 PPS, but **6.9 at 1200 and 142.5 at
3000**, at its own example pitch. The accumulation genuinely happens.

**What actually saves it** is something neither of us spotted: the re-sync *search* runs
**before** the short-circuit return. It re-fires within one spindle revolution, and its re-pin
discards the accumulated error and re-opens the gate — after which `sendPulse()` decays the
speed and everything latches. The error is discarded, never closed, so nothing lurches.

**And the guard is not vacuous** — mutating the re-sync condition to never fire reproduces
exactly the predicted failure: error growing 785.7 → 1573.6 pulses over 10 → 20 revolutions at
0.25 mm, which is precisely one pitch per revolution. Notably 300 PPS still passed under that
mutation, because that is the one speed where the planner really does land on zero. The
review's instinct that this would be "intermittent and much nastier" was right about the shape,
just not the outcome.

The existing arrest tests never exercised any of this: `RH_DecelerationLandsOnLeftEndstop`
unsets its stop, which drops the anchor, so `syncArmed()` is false throughout and the gate is
never live. Three new cases now cover it.

### A real, milder finding that came out of it

If the lathe is **stopped** right after an arrest that left residual speed, nothing drives the
phase onto the gate, so the rescue never runs and `m_leadscrewSpeed` stays frozen at its arrest
value **indefinitely** — measured still present after 20 s of idle. Re-engaging is safe (the
gate still holds, and its firing discards the error), but the next jog starts from a stale
non-zero speed instead of ramping from rest, and `getStoppingDistanceInPulses()` over-estimates
until it decays.

Related fragility: the rescue rests on an **exact-equality** phase match, so if the encoder ever
advances more than one count between `update()` iterations (high RPM plus a stalled loop), the
rescue is deferred a revolution at a time.

Either of two one-line changes closes both — decay the speed on the short-circuit path, or drop
the `m_leadscrewSpeed == 0` condition on the re-pin. **Not applied**: neither is needed for
safety, and both change motion behaviour, so they are yours to call.

## Superseded: the original defeat argument

The final review defeated one of the three safety gates, and the argument is worth reading
because it is not a coding slip.

`motionActive` — the predicate behind every gate — derives "is the carriage under power" from
the **commanded** motion mode. There appears to be a reachable state where that reads
`MM_DISABLED` while `Leadscrew::update()` is still accumulating into `m_expectedPosition`:

1. Setting a stop latches `syncPositionState`, so `syncArmed()` is permanently true.
2. A thread cut arrests on the endstop, setting `MM_DISABLED` **and** `SS_UNSYNC`.
3. `jogMode` is now false, so the accumulate keeps running every iteration.
4. The re-pin that would neutralise it is guarded by `m_leadscrewSpeed == 0`.
5. `m_leadscrewSpeed` only decays inside `sendPulse()` — and the re-sync gate's short-circuit
   `return` skips `sendPulse()` entirely.

If the arrest leaves **any** residual speed the state is self-sustaining: the following error
grows by a pitch per revolution, indefinitely, while the UI honestly reports "not under power".
Sync then renders live, fires, and races the lost-update window the code itself documents as
"only reachable while ENGAGED".

**The caveat that decides it:** if the deceleration planner always lands on exactly zero, the
re-pin fires every iteration and there is no leak. That could not be settled by inspection, so
it is being settled by a host test across several speeds and both thread hands — if it leaks at
some speeds and not others it is intermittent, which is worse.

**The predicate is the real lesson.** "Commanded mode" and "axis is quiescent" are different
questions, and only `Leadscrew` can answer the second. A gate built on the first is guessing.

## The sync action nearly shipped a carriage lurch

Worth reading even if you skip the rest, because it is the clearest case tonight of tests
passing being necessary and not sufficient.

`setSyncPoint()` raised `SS_SYNC` unconditionally — and raising `SS_SYNC` is exactly what
*releases* the re-sync gate. While that gate holds the axis, `update()` keeps accumulating
spindle motion into `m_expectedPosition` with the carriage frozen, so the following error grows
by **a whole pitch per revolution of holding**. Firing the gate normally is what discards that
accumulation; jumping it skipped the discard and handed the axis a large error to close at
maximum speed.

Measured on the host rig: a sync taken a quarter-revolution into a hold moved the carriage
**0.32 mm instantly, into the work**. Worst case approaches a full 1.27 mm pitch. All 13
original tests passed with this present.

Two things follow for the UI work:

- **The Sync menu tile must be gated on the axis being disengaged.** Spec §6 already says
  "against a stopped spindle" — that needs enforcing, not just documenting. There is also a
  residual lost-update race on `m_expectedPosition`, reachable only when syncing while engaged
  and gated, which the same gate closes. And an anchor sampled mid-cut is skewed by the servo
  following error anyway.
- **Do not "simplify" the sync tests.** The two headline "returns every whole pitch" cases
  survive a sign inversion of the anchor, because they never disengage — they only exercise the
  relative feed. The disengage/re-engage cases are the ones doing the work.

## The existing thread-sync tests never exercised the sync anchor

Found while writing the FS-E suite, and worth knowing independently of this branch.

`test/test_thread_sync/`'s `establishSyncAt()` sets a stop at position 0 (which anchors), then
immediately unsets it. With no other stop that sets `syncPositionState = UNSET`, so
`syncArmed()` is false for the entire run. The later re-arm happens with the carriage
elsewhere, and `setStop()` only anchors when the stop position equals the current carriage
position — so no anchor is recorded either.

**Every helix property that suite proves holds purely because the leadscrew is a faithful
relative feed. The anchor plays no part in any of it.** The re-sync block in `update()`
(~lines 300-327) was dead in every test in the repo; the FS-E probe was the first code to
execute it. The good news is that it works — it lands within ~1.3 leadscrew pulses of the
original helix across 300/1200/3000 PPS after a disengage plus a fractional turn.

The reason it was invisible: **the anchor's only observable effect is where the axis resumes
after `SS_UNSYNC`.** Nothing else in the system reads it, so any test that does not disengage
and re-engage cannot tell a correct anchor from a missing one. That is why the new suite leans
on a disengage/re-engage helper — worth knowing before anyone "simplifies" those tests.

Two smaller notes from the same investigation:

- `update()` can drop and re-raise `SS_SYNC` within a single call, so **`SS_SYNC` is not a
  proxy for "engaged"**.
- The `case UNSET:` arm in `update()`'s switch is dead code and its own comment says so
  ("Can we even hit this???"). It is the natural-looking slot for a manual anchor and must not
  be reused as-is — it returns `m_currentPosition`, which would make the anchor mean "wherever
  you are right now", i.e. no phase gate at all.

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
4. **The rotary encoder bypasses the focus model entirely.** `KeyArray::updateEncoderPos()`
   calls `next/prevFeedPitch()` directly regardless of `UiFocus`, so turning the encoder while
   the STOPS or JOG SPEED widget is open silently steps the pitch — which contradicts §1's
   "the arrows are the only actuators". Pre-existing, but it used to be masked by the button
   lock, and the new buttonpad unlocks at boot, so it is now live from power-on. It should
   route through `UiState` as `PitchNext`/`PitchPrev` like everything else. Is the encoder
   still fitted and used? That decides whether this is worth wiring or whether the code should
   go.
5. **Should ENABLE also dismiss an open widget?** It does not today. Engaging the leadscrew
   while the RATE widget is up leaves it on screen for up to 4 s with the arrows already
   inhibited. Spec §1 lists the leave conditions as OK / HALT / idle, so this is as designed —
   but it may feel wrong on the bench, and it is a one-line change.
6. **Should a run-to-stop be cancellable by the opposite arrow, or only the same one?** §3 says
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
