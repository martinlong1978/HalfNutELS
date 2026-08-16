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
| A | GlobalState feed/jog semantics | `lib/global_state`, `lib/config/config.h` | in progress |
| B | Carriage + stop position in mm | `lib/leadscrew`, `lib/config/latheconfig.*` | in progress |
| C | DRO datum resolution | `lib/dro` (new) | in progress |
| D | UI focus state machine | `lib/ui` (new) | in progress |
| E | Real sync action | `lib/leadscrew` | queued (after B) |
| F | LatheConfig fields + save API + CHECKVALUE | `lib/config`, `src/WebSettings.*` | queued |
| G | board.h remap + buttonpad rewrite + lock removal | `src/`, `lib/global_state` | queued |
| H | Icons I1 → A8 | `lib/display/icons` | queued |
| I | Theme palette + display rebuild | `lib/display` | queued |
| J | RTC boot-to-setup flag | `src/main.cpp` | queued |

## Questions for Martin

_(accumulated as they arise; nothing here blocks the work below it)_

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
