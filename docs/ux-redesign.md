# UX redesign — button panel and on-screen UI

Status: **design proposal**, branch `ux-redesign`. Nothing here is implemented yet.

Goal: collapse nine top-level buttons down to a small set of first-class controls plus a
menu, and give the screen a coherent visual language. The panel stays a 3×3 matrix — only
the cap legends and the `board.h` `#define`s change.

---

## 1. The core idea: a focus model

Today every button is a verb. Nine verbs is too many, and the two most valuable keys
(left/right) are locked to a single verb each.

The redesign makes **◀ / ▶ the only actuators**, and the other keys choose *what the arrows
act on*. That one change is what buys back the panel.

> **Focus** is the field the arrows currently drive. It rests on **JOG** and returns there
> automatically.

| Focus | ◀ / ▶ do | Entered by | Leaves on |
|---|---|---|---|
| **JOG** (rest) | Move the carriage | default | — |
| **JOG SPEED** | Step the jog speed | `OK` | `OK`, `HALT`, 4 s idle |
| **RATE** | Step the pitch list | `RATE` | `OK`, `HALT`, 4 s idle |
| **MODE** | Cycle feed / thread-R / thread-L | `MODE` | `OK`, `HALT`, 4 s idle |
| **STOPS** | Set / clear the left / right stop | `STOPS` | `OK`, `HALT`, 4 s idle |
| **MENU** | Move through menu tiles | `MENU` | `MENU`, `HALT` |

### The rotary encoder

The encoder drives **whatever has focus**, with one deliberate exception:

| Focus | Encoder does |
|---|---|
| **Jog** (rest) | **Pitch** — *not* what the arrows do at rest. The encoder fills the gap, so pitch is adjustable without pressing `RATE` first. |
| Rate | Pitch |
| Jog speed | Jog speed |
| Mode | Mode |
| Menu | Move between tiles |
| **Stops** | **Inert.** A knob is far easier to nudge than a key, and committing an endstop position must stay a deliberate keypress. |

It routes through `UiState` like every key, so it obeys the same focus rules and the same
engaged-inhibit. It previously called `next/prevFeedPitch()` directly, bypassing all of it.

### ENABLE takes two presses when a widget is open

Engaging is a commitment to cut, and you should not commit while your attention is still inside
a picker. So with any widget or the menu open, the first `ENABLE` press **only dismisses it**;
a second press engages. At rest it engages immediately, as before.

`OK` has three jobs, and they never overlap:

| Gesture | Context | Effect |
|---|---|---|
| Click | Widget open | Done — commit and dismiss |
| Click | At rest | Open jog speed — the setting for what the arrows already drive |
| **Hold** | At rest | **Zero the DRO here** (§8) |

It is the only key that would otherwise sit idle at rest.

The focused field is always outlined in the accent colour and carries `◀ ▶` chevrons, so
"what will the arrows do right now" is answerable at a glance without pressing anything.

---

## 2. Panel layout

```
┌──────────┬──────────┬──────────┐
│   MODE   │   RATE   │  STOPS   │   selectors — choose what ◀ ▶ drive
├──────────┼──────────┼──────────┤
│    ◀     │    OK    │    ▶     │   the only actuators
├──────────┼──────────┼──────────┤
│   HALT   │   MENU   │  ENABLE  │   machine state
└──────────┴──────────┴──────────┘
```

Three rows with three distinct jobs. Selectors on top, actuators in the middle where the
thumbs sit, state on the bottom.

### Matrix mapping

The scan builds `code = a | b<<3` (`keyarray.cpp:125,137-138`) where `a` is the H bitmask
and `b` the V bitmask. **Physical rows run along H, physical columns along V** — so the
codes read down the H index and across the V index:

|        | V1 (left) | V2 (centre) | V3 (right) |
|--------|-----------|-------------|------------|
| **H1** (top) | `9` | `17` | `33` |
| **H2** (mid) | `10` | `18` | `34` |
| **H3** (bot) | `12` | `20` | `36` |

Today's caps sit on those codes like this — note Rate−/Rate+ adjacent on the top row and
Jog L/R adjacent on the bottom, the grouping this redesign breaks up:

|        | V1 | V2 | V3 |
|--------|----|----|----|
| **H1** | `9` Rate − | `17` Rate + | `33` Mode |
| **H2** | `10` Sync | `18` Half-nut | `34` Enable |
| **H3** | `12` Lock | `20` Jog ◀ | `36` Jog ▶ |

The Mk2 assignment is a pure `board.h` edit:

|        | V1 | V2 | V3 |
|--------|----|----|----|
| **H1** | `9` MODE | `17` RATE | `33` STOPS |
| **H2** | `10` ◀ | `18` OK | `34` ▶ |
| **H3** | `12` HALT | `20` MENU | `36` ENABLE |

Note `18` stays the centre key in both layouts, and it is today's `ELS_HALF_NUT_BUTTON` —
the key sampled at boot for setup mode (`main.cpp:120-129`). Putting **OK** there keeps the
boot gesture memorable: *hold OK at power-on for setup*.

### What happened to the old nine

| Old key | Fate |
|---|---|
| Rate increase / decrease | → `RATE` + arrows (one key freed) |
| Jog left / right | → bare arrows (one key freed) |
| Mode cycle | → `MODE` (unchanged, now opens a widget) |
| Enable | → `ENABLE` (unchanged) |
| Lock | **removed** — see §7 |
| Thread sync | → menu tile (its short-click was never implemented) |
| Half-nut | → `OK`; its OTA hold moves to a menu tile, its debug click is deleted |
| — | new: `STOPS`, `HALT`, `MENU` |

---

## 3. Arrows at rest — jog

With focus on JOG, the arrows are context-sensitive on whether that side has a stop:

| ◀ pressed | Left stop `SET` | Left stop `UNSET` |
|---|---|---|
| Click | Run under power to the left stop, then hold position | — |
| Hold | Jog while held at the selected manual jog speed, arresting at the left stop; decelerate in place on release | Continuous jog while held; decelerate on release |

▶ is the mirror. This unifies the two jog behaviours that today are split across a mode
(`FM_JOG` hold-to-move vs. non-jog click-to-stop) — the machine already knows which one you
mean, because it knows whether the stop exists.

The two gestures cannot collide: the keypad never emits a `Click` after a `Hold`
(`lib/keyscan`), so one press is either a run or a jog and never both.

Hold on a side with a stop is a **jog**, not a shorter way to say click. Releasing the arrow
decelerates the carriage where it is rather than letting it carry on to the stop, and the
speed is the operator's — `jogSpeedPps() * getJogSpeed()`, the same speed the stop-free jog
in the right-hand column runs at, not the click-run's fixed `jogSpeedPps()`. The multiplier
is the control the operator has for how fast a held jog moves; a jog that ignored it would
not be the feature.

Consequences:

- **`FM_JOG` disappears from the mode cycle.** `IncFeedMode()` becomes
  `FEED → THREAD → THREAD_REVERSE → FEED`. Jog is always available, never a mode.
- A second press of the same arrow during a powered run **cancels** it (today's
  `MM_DECELLERATE` behaviour, kept).
- Arrows are **inhibited while `MM_ENABLED`**. The state bar says why rather than silently
  ignoring the press.
- **Holding an arrow is no longer the escape from a misplaced stop on that side.** The
  dead-man jog (`MM_INTERACTIVE_JOG_*`) drives straight through a stop, and on a side with a
  stop set the hold now reaches `MM_HOLD_JOG_*` instead, which arrests at it. The remaining
  route off a stop that is in the wrong place is to **unset it first** — a hold on that arrow
  with `STOPS` open (§4) — which is permitted at rest, since stop edits are inhibited only
  under power. A real behaviour change, and arguably the safer one: the gesture it replaces
  drove a carriage through a limit towards the chuck.
- **Jog speed moves onto `OK`.** Today it rides on `next/prevFeedPitch()`, which dispatch to
  `inc/decJogSpeed()` only when the mode is `FM_JOG` (`globalstate.cpp:158-176`). With that
  mode gone it needs an explicit home: tap `OK` at rest, arrows step the speed, `OK` again
  returns. It keeps a menu tile as the discoverable route.
- While the jog-speed widget is open the arrows own the speed, not the carriage — so jogging
  pauses for as long as it is up. It is a two-tap in-and-out, not somewhere you linger.

---

## 4. The selector widgets

All three share one grammar: press the key, a solid panel replaces the centre of the
screen, arrows adjust, `OK` or 4 s idle dismisses. No animation, no translucency (see §8).

### MODE

Three tiles in a row — Feed, Thread ‌R, Thread L — current one lit. Arrows move, OK commits.

Improvement over today: `IncFeedMode()` currently ends with `setFeedSelect(-1)`
(`globalstate.cpp:70`), which throws away your pitch every time you change mode. Replace
with **four remembered indices**, one per (mode × unit) pair, so switching feed → thread →
feed returns you to the pitch you were using.

### RATE

The pitch list as a horizontal ticker, current value large in the centre, neighbours dimmed
either side. Arrows step, OK commits.

Display caution: `getCurrentFeedPitch()` returns mm/rev always and negates for
`FM_THREAD_REVERSE` (`globalstate.cpp:137-156`). Rendering "16 TPI" or "4 thou" must index
`threadPitchImperial[]` / `feedPitchImperial[]` directly via `getFeedSelect()`.

### JOG SPEED

Opened by tapping `OK` at rest. Six steps from `jogSpeeds[]`, shown as a percentage bar with
the value large in the centre. Arrows step, `OK` closes.

The scale is `{0.01, 0.05, 0.1, 0.25, 0.5, 1}` → 1 %, 5 %, 10 %, 25 %, 50 %, 100 % of
`jogSpeedPps()`. Two fixes go with the move:

- `incJogSpeed()` clamps against a **hardcoded 5** rather than `ARRAY_SIZE(jogSpeeds) - 1`
  (`globalstate.cpp:83-85`), so adding a speed silently makes it unreachable.
- The `FM_JOG` dispatch inside `next/prevFeedPitch()` goes away with the mode. Those functions
  also return a stale `m_feedSelect` on the jog branch today — a latent bug that disappears
  with the same change.

### STOPS

Shows the travel bar full-width with both stop markers and the live carriage position.

| Gesture | Effect |
|---|---|
| ◀ click, left stop unset | Set left stop **here** |
| ◀ click, left stop set | Flash "hold to clear" — no action |
| ◀ hold, left stop set | Clear the left stop |
| `STOPS` hold | Clear **both**, after a 1 s confirm bar |

The asymmetry is deliberate: setting a stop is cheap to undo, clearing one loses a position
you may have spent time finding. ▶ is the mirror.

Clearing re-anchors the helix onto the surviving stop — that logic already exists in
`LeadscrewStopSync::unsetStop` (`leadscrew.cpp:56-96`) and needs no change.

---

## 5. HALT and ENABLE

**HALT** is always live — it is checked before focus, before overlays, before anything. It
sets `MM_DECELLERATE`, closes any open widget, and returns focus to JOG. It is the one key
whose meaning never depends on context.

**ENABLE** toggles `MM_ENABLED` ↔ `MM_DECELLERATE`, exactly as today. The state chip on the
bottom bar is its readout.

---

## 6. MENU

`MENU` opens a horizontal carousel of tiles; arrows move between them, `OK` activates;
`MENU` again (or `HALT`) closes. A carousel rather than a vertical list because we only have
left/right keys, and a 320×240 landscape panel suits a row of cards.

**Tiles never edit in place.** `OK` either performs a one-shot action or closes the menu and
opens the matching overlay — the same overlays the selector keys open. There is no editing
sub-state: the arrows mean "move between tiles" for as long as the menu is up, full stop. One
grammar, and nothing new to learn.

| Tile | `OK` does | Notes |
|---|---|---|
| **Units** | Toggle mm / inch | `GlobalState` restores the per-(mode,unit) pitch slot itself — do **not** also reset the index |
| **Theme** | Toggle dark / light | Persisted. Refused while the carriage is under power |
| **DRO datum** | Opens the datum overlay | Persisted. Refused while under power |
| **Jog speed** | Opens the jog-speed overlay | The same widget `OK` opens at rest; here for discoverability |
| **Sync** | Sets a sync point | Disabled in feed mode **and while the axis is under power** — see §9 item 12 |
| **Software update** | `setOTA()` | Replaces the half-nut hold. Refused under power |
| **Setup / Wi-Fi** | Reboots into AP mode | Refused under power. See below |
| **Diagnostics** | Live position error, spindle vs leadscrew rates, sync anchor state | Replaces the dead serial debug mode, which stays dead |
| **About** | Firmware version, IP, uptime | Read-only |

### Lathe geometry is web-only

Encoder PPR, stepper PPR, `invertDirection`, the gearbox ratio, leadscrew pitch, jog speed,
acceleration and max speed are **commissioning values**: set once over Wi-Fi with the lathe
offline, and changed only there. **The device must never write them.** The menu covers only what
is worth tweaking mid-session.

This is enforced by the shape of the API, not by discipline: `saveLathePreferences(theme, datum)`
takes values, so there is no struct a caller could get wrong. Geometry exists only as bytes read
back from flash inside that function. The predecessor took a `LatheConfig*` and wrote
`sizeof(LatheConfig)`, so a theme toggle rewrote all nine geometry fields out of an in-RAM
reconstruction — and a stale copy, a missed field or an unseeded struct would each have silently
replaced the user's commissioning.

### Auto-return is rejected

Automatically running back to the other stop after a pass **only works with a servo on the
cross-slide to retract the tool**. Without one the return pass is still cutting and mars the
thread just made. This is a machining fact, not a UI preference. Do not re-propose it.

### There is no separate "zero on set"

Setting the datum-preferred stop already makes it the datum, which already reads `0.00`. A
preference for it would be a no-op field — and a `LatheConfig` addition, which costs a
`CHECKVALUE` bump and wipes every device's Wi-Fi. Considered and dropped.

### Setup tile needs a reboot flag

There is currently no runtime path into config mode: `runWifiSettings()` is file-local in
`main.cpp` and called only from `setup()`. Writing a flag to flash is unattractive because
`WebSettings` and `LatheConfig` share one 4 KB sector that is erased as a unit.

Use **RTC memory** instead — `RTC_DATA_ATTR uint32_t bootToSetup;` survives `ESP.restart()`,
costs no flash wear, and clears on power-cycle. `setup()` then checks
`digitalRead(ELS_PAD_H2) || !checks || bootToSetup == MAGIC`.

### Saving settings from the device

Theme and DRO datum are the first settings the *device* has ever needed to write. Today the
only writer is `setValues()` in `WebSettings.cpp:254-282` — file-local, and dependent on the
live `webServer` global for its argument parsing. So it needs a small exported API:

```cpp
void saveLatheSettings(LatheConfig* config);   // read-modify-write the shared sector
```

It must be read-modify-write: `WebSettings` and `LatheConfig` live in **one 4 KB sector that
is erased as a unit** (`WebSettings.cpp:16-18`), so writing lathe settings without first
re-reading the Wi-Fi credentials would wipe them.

**Adding fields to `LatheConfig` requires bumping `CHECKVALUE`.** The stored blob is shorter
than the new struct, so `droDatum` and `theme` would otherwise read back as flash garbage.
Per `CLAUDE.md`, bumping invalidates saved Wi-Fi *and* lathe parameters, so the device boots
into AP setup and everything must be re-entered — schedule it with the firmware that ships
the redesign, not as a separate release.

---

## 7. Removing the lock

Lock exists to stop an accidental keypress doing something dangerous. It is worth asking
what is actually dangerous:

- **Feed and thread motion is inherently safe when the spindle is stopped.** The leadscrew
  only ever moves in sync with the spindle, so a stray `ENABLE` with the chuck stationary
  moves nothing. A stray pitch change is instantly visible and instantly reversible.
- **Jog is the only thing that moves metal unprompted** — and jog is either hold-to-move
  (dead-man, safe by construction) or a powered run to a stop *you already set*, cancellable
  by any of three keys.

So the lock is guarding the safe operations and not the risky one. Removing it costs
nothing and frees a key. `HALT` is the better answer to the same worry: rather than
preventing motion from starting, make stopping it unmissable.

`GlobalButtonLock` can be deleted outright, along with the lock checks at the top of every
handler in `buttonpad.cpp` and `drawLocked()` in the display.

---

## 8. Visual design

### Layout — 320×240 landscape

```
 0 ┌────────────────────────────────────────────┐
   │ ⚙ THREAD·R    mm    ◈sync         1250 RPM │  status bar
30 ├────────────────────────────────────────────┤
   │                                            │
   │      ◀   1.25 mm   ▶            ╱╱╱╱       │  primary readout
   │          ▔▔▔▔▔▔▔▔▔              ╱╱╱╱       │  (focus underline)
120├────────────────────────────────────────────┤
   │  ·  ·  ·  ·  ▮  ·  ·  ·  ·  ·  ·  ·  ·  ·  │  pitch ticker
140├────────────────────────────────────────────┤
   │   ▐━━━━━━━━━━━●─────────────────────▌      │  carriage travel
   │  L 0.0           12.4 mm         48.0 R    │  (a small DRO)
190├────────────────────────────────────────────┤
   │  ● RUNNING        HALT   MENU   ENABLE     │  state + soft keys
240└────────────────────────────────────────────┘
```

The **carriage travel bar** is new and is the biggest information win: stop positions and
where the carriage sits between them are currently invisible, and they are what you actually
need to know mid-cut. It reads as a small DRO — live position and unit in the centre, each
stop's own position at its end, `——` where a stop is unset. It needs one plumbing change (§9).

### Units

Every number carries its unit, sized down and dimmed so the digits still dominate. They are
not all the same unit:

| Readout | Metric | Imperial |
|---|---|---|
| Pitch — thread | `1.25 mm` | `16 TPI` |
| Pitch — feed | `0.25 mm` | `4 thou` |
| Carriage & stops | `12.4 mm`, 2 dp | `0.488 in`, 3 dp |
| Jog speed | `25 %` | `25 %` |
| Spindle | `1250 RPM` | `1250 RPM` |

Feed in thou/rev while position reads in inches is not an inconsistency — it is what machine
tools do, and matching it is correct. The status bar keeps its `mm` / `inch` chip as the one
place that states the unit mode itself.

Imperial text **cannot** come from `getCurrentFeedPitch()` — see the display caution in §4.
Position is converted for display only; internally everything stays in pulses.

### The DRO datum

A position is meaningless without a zero, and "wherever the machine happened to boot" is the
worst available choice. **Zero is referenced to an endstop.**

Resolution order, first match wins:

| # | Condition | Datum |
|---|---|---|
| 1 | Manual zero set (`OK` held) | That position |
| 2 | Both stops set | The one named by `LatheConfig.droDatum` |
| 3 | Exactly one stop set | That one — the preference cannot be honoured |
| 4 | Neither stop set | Power-on origin, flagged `REL` on screen |

**Sign convention: position increases to the right**, matching
`LeadscrewDirection::RIGHT = 1`. So a left datum reads 0 at the left and counts up rightward;
a right datum reads 0 at the right and counts *negative* leftward. Negative travel is normal
DRO behaviour and needs no special casing.

The datum end shows `0.0` and is underlined; the far end shows the span. A manual zero adds a
`▽` tick on the bar at the zero point and tags the readout `MAN`.

Four details the rules above leave implicit, pinned by `test/test_dro/` and repeated here so
they are not lost in test code:

- **A stop parked at pulse 0 is a real datum.** "Is it set" comes from the stop state alone,
  never from `pulses != 0`. This is the trap an implementation falls into most easily.
- **An unset stop's stored position must be ignored entirely** — it may hold stale junk.
- **Coincident stops** (both set to the same position) still resolve to the preferred *side*,
  because the chosen source is what decides which end of the bar renders as the datum.
- **Translating the `INT32_MIN` / `INT32_MAX` unset sentinels into set/unset flags is the
  caller's job.** `lib/dro` takes explicit booleans and never sees a sentinel.

Two consequences worth designing for:

- **The numbers jump when the datum changes.** Setting a second stop can hand the datum to the
  configured side. This is inherent in the rule, not a bug — but it must not be silent, so the
  readout flashes for ~1 s whenever the datum moves.
- **Manual zero outranks everything** until it is cleared, and it is cleared by re-picking
  Left or Right on the *DRO datum* menu tile. That keeps one authority for "what is zero"
  rather than two competing ones.

### Theme

Dark and light are both good on this panel in different light, so the choice is the user's:
a **Theme** menu tile, persisted in `LatheConfig`.

Implementation is cheaper than it looks, given one asset change:

- Replace the `#define COLOUR_*` constants with a palette struct and a pointer to the active
  one. Both palettes stay compiled in; they are a few dozen bytes.
- On change, call the existing `setDisplayReset()` path — `Display::update()` already rebuilds
  the whole screen when it sees that flag (`ST7789_320_240displaylvgl.cpp:255-256`). No new
  mechanism needed.
- **Regenerate the icons as `A8` instead of `I1`.** This is the load-bearing part. LVGL applies
  recolour for free *only* on A8, where the image is treated as a mask and the blend colour is
  set directly (`lv_draw_sw_img.c:240-252`); every other format with `recolor_opa > 0` falls
  through to `recolor_only()`, which allocates a temp buffer on every draw out of the 64 KB
  LVGL pool. With A8 the same twelve assets serve both themes via
  `lv_obj_set_style_image_recolor()`, and the edges come out anti-aliased instead of jagged.

  Flash cost: A8 is 8 bytes per pixel-byte of I1, so 4 × 128×64 + 8 × 32×32 goes from
  **5.2 KB to 40 KB**. There is ~519 KB of headroom, so this is affordable and it removes the
  need for duplicate light/dark artwork entirely.

The bottom-right soft-key hints mirror the bottom physical row, so the panel is
self-documenting for the two functions that have no on-screen state of their own.

### Type scale

Only Montserrat **14 / 26 / 36 / 48** are compiled in (`lv_conf.h:606-626`), so:

| Role | Size |
|---|---|
| Pitch value | 48 |
| RPM, state word | 36 |
| Chips, travel readout | 26 |
| Labels, hints | 14 |

Adding e.g. `LV_FONT_MONTSERRAT_20` is a one-line `lv_conf.h` change if the 14→26 jump
proves too coarse.

### Colour

**Author every colour pre-swapped.** The panel is R↔B swapped and the existing `COLOUR_*`
constants already compensate (`CLAUDE.md:84-87`).

| Role | Natural RGB | **Write this** |
|---|---|---|
| Run / armed | `#00C853` | `0x53C800` |
| Jog / caution | `#FFAB00` | `0x00ABFF` |
| Halt / fault | `#FF3B30` | `0x303BFF` |
| Idle / inactive | `#6B7280` | `0x80726B` |
| Focus accent | `#38BDF8` | `0xF8BD38` |

Both a dark and a light palette are mocked; the accent/state hues are shared so only the
ground and text tokens differ.

### Renderer constraints

These come from `lv_conf.h` and are hard limits, not preferences:

- **No translucent overlays.** Layer opacity allocates an intermediate buffer bounded by
  `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` = 24 KB; a 300×140 RGB565 panel needs ~84 KB. Overlay
  panels must be **solid fill**.
- **No animation.** `DisplayTask` sleeps 100 ms (`main.cpp:75`), so everything runs at
  10 FPS and any transition looks steppy. Widgets appear instantly. (Dropping the delay to
  33 ms is viable — display is priority 1 on core 1, motion is priority 24 on core 0 — but
  it is a separate change with its own measurement.)
- **Gradients: linear only, 2 stops max** (`LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0`,
  `LV_GRADIENT_MAX_STOPS 2`).
- **Shadows are uncached** (`LV_DRAW_SW_SHADOW_CACHE_SIZE 0`) — recomputed on every redraw.
  Use small blur radii or none.
- **Few distinct corner radii** — only 4 are cached (`LV_DRAW_SW_CIRCLE_CACHE_SIZE 4`).
- **`msgbox`, `dropdown`, `roller`, `switch`, `checkbox` are compiled out.** Every widget
  here is built from `lv_obj` + `label` + `bar` + `line`, all of which are enabled. Confirm
  dialogs are hand-rolled panels, not `msgbox`.

### Icons

Assets are 1-bit indexed with the foreground colour **baked into an 8-byte palette**, and
there is no `lv_image_set_recolor` call anywhere. The 32×32 status icons are already white
(fine for dark); the four 128×64 mode symbols are black and need their palette index-1 bytes
flipped for a dark theme — 8 bytes per file, no regeneration needed.

New glyphs (menu, halt, settings) should be **text**, not icons, to avoid the manual
converter round-trip. There is ~519 KB of flash headroom if that changes.

---

## 9. Firmware work implied

Roughly in dependency order. None of it is on the motion hot path.

1. **Carriage position in mm.** `Leadscrew::getCurrentPosition()` is in pulses, `config` is
   a private member with no accessor, and the only `LatheConfigDerived` is a local in
   `setup()` (`main.cpp:141`). Add `float getPositionMM()` on `Leadscrew` (or hoist the
   config to a global and pass it to `Display`). Same requirement for showing stop positions
   — `getStopPosition()` already exists and has no non-test callers, and returns the
   sentinels `INT32_MIN` / `INT32_MAX` when unset, which is exactly what renders as `——`.
   Imperial divides by 25.4 at the point of render only; internally everything stays in
   pulses.
2. **A UI state machine.** Focus + overlay state, currently nonexistent. Belongs beside
   `ButtonPad`, not in `GlobalState` — it is display-task-local and needs no cross-core
   visibility.
3. **`board.h` remap** to the table in §2.
4. **Rewrite `buttonpad.cpp`** around focus rather than one handler per verb.
5. **Delete** `GlobalButtonLock`, `drawLocked()`, and the dead debug path (`setDebugMode()`
   is an empty function body, and `debugBuffer` is an uninitialised pointer written from the
   4 KB spindle task — a latent crash if anyone re-enables it).
6. **Per-mode pitch memory** (§4) and the `setUnitMode()` index reset (§6).
7. **RTC boot-to-setup flag** (§6).
8. **DRO datum resolution** (§8) — pure arithmetic over `getStopPosition()` and the config
   preference, plus the manual-zero override on `OK`-hold. Worth host tests: the resolution
   order has four branches and a sign convention, and it is cheap to cover.
9. **Theme support** (§8) — palette struct, `LatheConfig` fields, `saveLatheSettings()`, and
   the `CHECKVALUE` bump.
10. **Regenerate the twelve icons as `A8`** (§8). Prerequisite for the theme; also worth doing
    on its own for the anti-aliasing.
11. **Rebuild the display** to the §8 layout, plus overlay rendering.
12. **A real sync action** (§6) — the only genuinely new motion behaviour, and the only item
    that must have host-test coverage before it goes near the lathe.

Items 1–7 and 9–11 are UI-layer and testable by inspection. Items 8 and 12 should follow the
repo's test-first convention (`CLAUDE.md`, "Testing conventions").
