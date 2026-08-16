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
| **RATE** | Step the pitch list | `RATE` | OK, `HALT`, 4 s idle |
| **MODE** | Cycle feed / thread-R / thread-L | `MODE` | OK, `HALT`, 4 s idle |
| **STOPS** | Set / clear the left / right stop | `STOPS` | OK, `HALT`, 4 s idle |
| **MENU** | Move through menu tiles | `MENU` | `MENU`, `HALT` |

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
| Hold | (same as click) | Continuous jog while held; decelerate on release |

▶ is the mirror. This unifies the two jog behaviours that today are split across a mode
(`FM_JOG` hold-to-move vs. non-jog click-to-stop) — the machine already knows which one you
mean, because it knows whether the stop exists.

Consequences:

- **`FM_JOG` disappears from the mode cycle.** `IncFeedMode()` becomes
  `FEED → THREAD → THREAD_REVERSE → FEED`. Jog is always available, never a mode.
- A second press of the same arrow during a powered run **cancels** it (today's
  `MM_DECELLERATE` behaviour, kept).
- Arrows are **inhibited while `MM_ENABLED`**. The state bar says why rather than silently
  ignoring the press.
- Jog speed is no longer reachable via `next/prevFeedPitch()`. It becomes a preference:
  `RATE` **held** opens the jog-speed picker, and it also has a menu tile.

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

`RATE` **held** switches the same widget to jog speed (`jogSpeeds[]`, shown as 1 %–100 %).

Display caution: `getCurrentFeedPitch()` returns mm/rev always and negates for
`FM_THREAD_REVERSE` (`globalstate.cpp:137-156`). Rendering "16 TPI" or "4 thou" must index
`threadPitchImperial[]` / `feedPitchImperial[]` directly via `getFeedSelect()`.

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

| Tile | `OK` does | Notes |
|---|---|---|
| **Units** | Toggle mm / inch | Must also `setFeedSelect(-1)` — `setUnitMode()` is a bare assignment (`globalstate.cpp:182`) and does not reset the index |
| **Jog speed** | ◀ ▶ adjust inline | Also on `RATE`-hold |
| **Sync** | Set a sync point against a stopped spindle | Disabled in Feed mode. Needs real work — today the anchor is only ever latched as a side effect of setting a stop |
| **Software update** | Confirm → `setOTA()` | Replaces the half-nut hold |
| **Setup / Wi-Fi** | Confirm → reboot into AP mode | **Needs new firmware** — see below |
| **Diagnostics** | Live position error / pulse counts on screen | Replaces the dead serial debug mode |
| **About** | Firmware version, IP, uptime | Read-only |

### Setup tile needs a reboot flag

There is currently no runtime path into config mode: `runWifiSettings()` is file-local in
`main.cpp` and called only from `setup()`. Writing a flag to flash is unattractive because
`WebSettings` and `LatheConfig` share one 4 KB sector that is erased as a unit.

Use **RTC memory** instead — `RTC_DATA_ATTR uint32_t bootToSetup;` survives `ESP.restart()`,
costs no flash wear, and clears on power-cycle. `setup()` then checks
`digitalRead(ELS_PAD_H2) || !checks || bootToSetup == MAGIC`.

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
   │  L▐━━━━━━━━━━━●─────────────────────▌R     │  carriage travel
   │    set              12.4 mm      unset     │
190├────────────────────────────────────────────┤
   │  ● RUNNING        HALT   MENU   ENABLE     │  state + soft keys
240└────────────────────────────────────────────┘
```

The **carriage travel bar** is new and is the biggest information win: stop positions and
where the carriage sits between them are currently invisible, and they are what you actually
need to know mid-cut. It needs one plumbing change (§9).

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
   — `getStopPosition()` already exists and has no non-test callers.
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
8. **Rebuild the display** to the §8 layout, plus overlay rendering.
9. **A real sync action** (§6) — the only genuinely new motion behaviour, and the only item
   that needs host-test coverage before it goes near the lathe.

Items 1–8 are UI-layer and testable by inspection. Item 9 should follow the repo's
test-first convention (`CLAUDE.md`, "Testing conventions").
