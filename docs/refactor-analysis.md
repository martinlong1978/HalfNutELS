# HalfNutELS - Component Design Analysis & Refactor Plan

Prioritized, constructive analysis of the firmware's component design, with concrete
directions and risk/effort estimates. This project was assembled from someone else's
partly-broken code, so the intent here is to chart a safe path forward, not to assign
blame.

Scope notes:

- **Bugs are report-only.** The confirmed regression and robustness findings are already
  documented in `docs/websettings-review.md` and are **not** duplicated here. This document
  is about *design*, not defects.
- **Part B (applied refactors)** is summarized at the end. Only changes provable-safe by the
  passing pinning tests (`test_latheconfig`, `test_leadscrew`, `test_smoke` = 18 tests) were
  applied; `test_regression` remains failing **by design**.
- Every claim below was verified in the source; citations are `file:line` against the current
  working tree.

---

## Priority summary

| # | Area | Severity | Effort | Group |
|---|------|----------|--------|-------|
| A1 | Extract repeated positive-modulo idiom | low | trivial | safe/mechanical (**DONE**) |
| A2 | Remove stale/dead code (`TestSpindle.h`, `//m_accumulator`) | low | trivial | safe/mechanical (**DONE**) |
| A3 | Commit the native test harness; fix the stale committed test double | **high** | low | safe/mechanical |
| A4 | Build/config hygiene (name, Teensy vestiges, `ELS_USE_RMT`) | medium | low-med | medium |
| A5 | `LatheConfigDerived` getter boilerplate + precompute derived values | medium | medium | medium |
| A6 | Abstract spindle/encoder interface (remove the .cpp-swap test hack) | medium | medium | medium |
| A7 | Split `GlobalState` god-singleton; make the task hand-off thread-safe | high | large | large/architectural |
| A8 | Decompose the `Leadscrew` god object | high | large | large/architectural |
| A9 | Rework the WebSettings model (form/parse/persist registry) | high | large | large/architectural (do NOT touch now) |

---

## Group 1 - Safe / mechanical

### A1. Repeated positive-modulo idiom (DONE in Part B)

**Problem.** The Euclidean "always-positive modulo" idiom `((x % m) + m) % m` was copy-pasted
at four sites, three of them in deeply-nested one-liners:

- `lib/leadscrew/leadscrew.cpp:60`, `:69`, `:299` (wrapping into `encoderPPR`)
- `lib/spindle/TestSpindle.cpp:38` (wrapping into `ppr`)

**Why it matters.** The idiom is easy to get subtly wrong, and in fact the hardware spindle
diverges from it: `lib/spindle/ESPSpindle.cpp:49` uses `(position + ppr) % ppr`, which is only
correct for `position >= -ppr` and can return a negative result otherwise. So the *tested*
double and the *shipped* code compute wrapping differently - a testing-fidelity gap hiding
behind duplicated math. (The ESP divergence is a latent bug; left report-only.)

**Direction (applied).** Added a single well-named helper
`inline int positiveModulo(int value, int modulus)` to `lib/config/config.h:23` and used it at
the four sites with **identical integer semantics**. The `ESPSpindle.cpp` site was deliberately
left alone (different semantics, not covered by the native tests) - unifying it is a behavioral
change that belongs behind the fix, not this refactor.

**Risk/effort:** trivial / verified by 18/18 passing tests.

### A2. Dead and stale code (DONE in Part B)

- `lib/spindle/TestSpindle.h` - **removed.** It declared an obsolete `TestSpindle` class with a
  no-arg constructor and referenced the deleted macro `ELS_SPINDLE_ENCODER_PPR`. Nothing
  included it (verified by grep); it would not compile if it were.
- `//m_accumulator` - **removed** the two inert commented-out fragments
  (`lib/leadscrew/leadscrew.h:67`, `lib/leadscrew/leadscrew.cpp:27`). The accumulator concept
  was abandoned; the leftover comments mislead.

**Remaining, NOT applied** (kept as proposals to keep the diff minimal and mechanical):

- `lib/leadscrew/leadscrew.cpp:272` - a commented-out `// m_currentDirection = ...` inside an
  empty `else` branch. Removing it would leave a bare empty `else`; it arguably documents the
  deliberate decision *not* to reset direction there. Left as-is.
- The stale doc comments referring to "the accumulator" (`leadscrew.h:80-83`,
  `leadscrew.cpp:336-337`) - rewording is a judgement call, not obviously inert.
- The entirely commented-out body of `GlobalState::setDebugMode` (`globalstate.cpp:18-48`) -
  see A7; this is why the debug path is dead (below).

### A3. The native test harness is uncommitted, and the committed test double is broken

**Problem.** This is the most urgent hygiene issue and was discovered while running Part B.
The whole host test harness is **untracked / uncommitted**: `test/test_*`, `test/stubs/`,
`scripts/exclude_espspindle_native.py`, and `docs/` all show as `??` in `git status`. Worse,
the *committed* `HEAD:lib/spindle/TestSpindle.cpp` is a **stale, non-compiling** version
(`Spindle::Spindle()` no-arg ctor, references the removed `ELS_SPINDLE_ENCODER_PPR`,
`consumePosition()` returns `-1`). The working, tests-passing test double exists **only as an
uncommitted modification** to that tracked file.

**Why it matters.** A fresh checkout of `HEAD` cannot build the native tests. The green test
suite depends entirely on files that are not in version control - one `git checkout`/`git clean`
away from being lost. (That is exactly what happened during this task; the double was
reconstructed verbatim and re-verified.)

**Direction.** Commit the harness and replace the stale committed `TestSpindle.cpp` with the
working double in the same commit. Consider renaming it (e.g. `SpindleNative.cpp`) so the
filename stops implying a separate `TestSpindle` class. Add a one-line CI job that runs
`pio test -e native` so this cannot silently rot again.

**Risk/effort:** low / mechanical, but high value.

---

## Group 2 - Medium

### A4. Build / config hygiene

- **Misleading project name.** The repo, and the `platformio.ini` envs `teensy41` /
  `teensy41_debug` (`platformio.ini:14-41`), still advertise Teensy, but Teensy support was
  removed in `cbd1b22`. The live targets are ESP32 (`esp32dev_publish`, `esp32dev_usb`) and
  `native`. The Teensy envs pull `lib_deps` (Adafruit GFX/SSD1306, elapsedMillis) no longer
  used by the ESP32 build. **Direction:** delete the Teensy envs, or gate them clearly as
  unsupported; rename the project to something ESP32-accurate.
  **(DONE)** The Teensy envs are gone from `platformio.ini` and the project was renamed to
  **HalfNutELS** - after the half-nuts an electronic leadscrew makes redundant, rather than after
  whichever microcontroller it happens to run on.
- **Vestigial `leadscrew_io_teensy.h`.** `lib/leadscrew/leadscrew_io_teensy.h` is entirely under
  `#ifdef CORE_TEENSY` (never defined) - dead. It is a clean, self-contained file, so deletion
  is trivial, but it is *not* exercised by any test, so I left it (out of the "provable-safe"
  envelope). **Direction:** delete alongside the Teensy envs. **(DONE)** - the file no longer exists.
- **`ELS_USE_RMT` is always on.** `board.h:19` unconditionally defines it, so the non-RMT
  `sendPulse()` branch (`leadscrew.cpp:144-154`) and the `pinMode(ELS_LEADSCREW_STEP, OUTPUT)`
  fallback (`main.cpp:176`) are dead on hardware. Note the native tests actually exercise the
  *RMT* branch (via a stubbed `rmtWrite` in `test/stubs/Arduino.h`), so the non-RMT branch has
  **no coverage** - do not "clean it up" blindly. **Direction:** either commit to RMT and delete
  the fallback, or make `ELS_USE_RMT` a real build option with a test that covers the other path.
- **Shared magic number.** `CHECKVALUE` (`latheconfig.h:6`) is a single 32-bit magic reused as
  the validity flag for two *different* flash structs (`WebSettings` and `LatheConfig`) with no
  version tag - see A9 and review Finding 5.

**Risk/effort:** low-med / mostly deletions; each is independently testable via a native build.

### A5. Config model: `LatheConfig` POD + `LatheConfigDerived` pass-throughs

**Current shape.**

- `LatheConfig` (`latheconfig.h:9-21`) - a POD struct of raw settings with in-class defaults,
  blitted to/from flash.
- `LatheConfigDerived` (`latheconfig.h:24-49`, `latheconfig.cpp`) - wraps a `LatheConfig*` and
  exposes **nine** hand-written pass-through getters (`latheconfig.cpp:10-36`) that add nothing
  but `return config->field;`, plus the genuinely-derived math (`:38-60`).
- Compile-time constants live in `config.h`; pins/board macros in `board.h`.

**Problems.**

1. **Getter boilerplate.** The nine pass-throughs are pure noise; they exist only so the derived
   math can call `stepperPpr()` instead of `config->stepperPpr`. Any new field means editing the
   struct, the class declaration, and the `.cpp` - three places for zero behavior.
2. **Derived values recomputed every call.** `leadscrewStepsPerMm()` and everything built on it
   (`jogSpeedPps`, `leadscrewMaxSpeedPps`, `accellerationPulseSec`, `leadscrewInitialPulseDelay`)
   are recomputed on each access (`latheconfig.cpp:38-56`) though the inputs only change on a
   settings write. In the `Leadscrew` hot path these are read via `config->...()` inside
   `update()` (`leadscrew.cpp:373,379,391`).
3. **Derived math is where the headline regression lives** - `leadscrewStepsPerMm()`
   (`latheconfig.cpp:38-40`) dropped the `/ leadscrewPitchMm` divisor. Report-only; see review
   Finding 1. It is called out here only because it is *structural*: the derived-math layer is
   the right home for a fix, and consolidating it (below) makes such regressions easier to catch.

**Direction.**

- Drop the pass-through layer. Either give `LatheConfig` the derived methods directly, or expose
  the fields and make the derived quantities **free functions** taking a `const LatheConfig&`.
- Compute the derived set **once** (on load / on settings change) into a small immutable
  `MotionParams` value object that `Leadscrew` and `Spindle` receive by value/const-ref, instead
  of holding a mutable `LatheConfigDerived*` and re-deriving in the ISR-driven loop.
- Add zero-guards at the derivation boundary (division by `gearboxRatioDenominator`,
  `spindleEncoderPpr`) - see review Finding 3.

**Risk/effort:** medium. The pinning tests cover default and a couple of non-default configs
(`test_latheconfig.cpp`), so a same-semantics consolidation is verifiable; changing *when*
values are computed touches `main.cpp` wiring and needs care.

### A6. Hardware coupling: no spindle/encoder seam

**Problem.** `Spindle` is a concrete class that embeds an `ESP32Encoder` by value
(`spindle.h:1,15`) and talks to GPIO in its ctor (`ESPSpindle.cpp:7-18`). `Leadscrew` depends on
the **concrete** `Spindle*` (`leadscrew.h:48`; used at `leadscrew.cpp:176,204,208,295`). There is
no interface seam, so to test `Leadscrew` on the host the harness had to (a) stub the encoder
header (`test/stubs/ESP32Encoder.h`) and (b) swap out the entire `.cpp` via a PlatformIO build
middleware that drops `ESPSpindle.cpp` and compiles a hand-written double instead
(`scripts/exclude_espspindle_native.py` + `lib/spindle/TestSpindle.cpp`). That is a lot of
scaffolding to work around a missing abstraction - and it is why the double can silently drift
from the hardware (A1).

**Direction.** Extract a narrow interface for what `Leadscrew` actually consumes from the
spindle: `getCurrentPosition()`, `consumePosition()`, `getEstimatedVelocityInPPS()`. Note the
codebase already has an abstract `RotationalAxis` (`axis.h:52`) - either widen that seam or add a
dedicated `ISpindle`. `Leadscrew` then depends on the interface; the hardware `Spindle`
implements it; a plain in-memory fake implements it for tests. This **removes the need for both
the encoder stub and the `.cpp`-swap script**, and lets the test double be a first-class class
again rather than an alternate definition of `Spindle::`.

**Risk/effort:** medium. Mechanically it is "introduce interface + change one pointer type,"
which the existing `LeadscrewIO` seam (`leadscrew_io.h`) shows is a proven pattern here. The
risk is churn in the build scripts and ctor wiring in `main.cpp:142-150`.

---

## Group 3 - Large / architectural

### A8. The `Leadscrew` god object

The class admits it itself (`leadscrew.cpp:11-15`: "This is kind of a god object..."). A single
class (`leadscrew.h:38`, inheriting `LinearAxis`, `DerivedAxis`, `DrivenAxis`) currently mixes
**five** distinct responsibilities:

1. **Position & stop model** - `m_currentPosition`, `m_expectedPosition`, `getPositionError()`
   (`leadscrew.cpp:428-430`), the left/right stop state/positions and their API
   (`leadscrew.cpp:51-131`), endstop detection (`leadscrew.cpp:197-201,352-355`).
2. **Spindle-sync model** - `m_syncPositionState`, `m_spindleSyncPosition`, sync extrapolation
   (`unsetStopPosition`, `leadscrew.cpp:56-71`) and re-sync search (`update`,
   `leadscrew.cpp:280-307`). This is where the modulo idiom clustered (A1).
3. **Motion profile / acceleration** - `m_leadscrewSpeed`, `m_currentPulseDelay`,
   `getStoppingDistanceInPulses()` (`:164-167`), `getTargetSpeedDistanceInPulses()` (`:175-188`),
   and the accel/decel/clamp state machine (`:360-423`).
4. **Pulse IO / timing** - `sendPulse()` (`:138-156`, incl. the RMT vs GPIO fork), the
   per-pulse timing gate (`:316-322`), velocity estimation (`:436-439`).
5. **Cross-cutting glue** - reads/writes `GlobalState` motion mode and thread-sync state
   throughout `update()`, and pokes the debug ring buffer (`:397-411`).

**Why it matters.** `update()` is ~230 lines of interleaved concerns with deep conditionals; it
is the ISR-adjacent hot path, the hardest thing to reason about, and the thing most likely to
regress. Every new feature widens it.

**Direction (decomposition).** The author's own TODO already points the way. Suggested seams:

- **`LeadscrewPosition` / stop-and-sync model** - owns current/expected position, stops,
  endstops, and the spindle-sync anchor; pure integer/float math, trivially unit-testable. The
  `positiveModulo` helper (A1) naturally lives near here.
- **`MotionProfile`** - owns speed, pulse delay, accel/decel; input = target speed + should-stop
  decision, output = next pulse delay. No knowledge of GlobalState or IO.
- **`PulseOutput`** - wraps `LeadscrewIO` / RMT (the `LeadscrewIO` seam already exists and is
  clean - reuse it).
- A thin **`Leadscrew` coordinator** that wires the three together per tick and is the only thing
  that talks to `GlobalState`.

Do this **incrementally, behind the pinning tests**, extracting one collaborator at a time and
re-running `test_leadscrew` after each. The current tests pin stop semantics, jog motion,
spindle-follow, and endstop-disable - a useful (if partial) safety net. Add characterization
tests for the accel/decel and re-sync paths *before* touching them.

**Risk/effort:** large / high risk. This is the core algorithm; sequence it last, after A6 gives
a clean spindle fake and after coverage is broadened.

### A7. `GlobalState` god-singleton and thread-safety

**Problem.** `GlobalState` (`globalstate.h:61`) is a classic singleton used as a global mutable
bus. It mixes at least four unrelated concerns:

- **UI/mode state** - feed mode, unit mode, button lock, jog speed/index, feed-select
  (`globalstate.h:70-81`).
- **Motion state** - motion mode, thread-sync state, resync pulse count (`:71,73,87`).
- **OTA state** - `OTA`, `OTAbytes`, `OTAlength` (`:64-66`).
- **Debug plumbing** - two **public** raw `volatile DebugData*` pointers `debugBuffer` /
  `debugInit` (`:101-102`) that `Leadscrew::update()` increments directly
  (`leadscrew.cpp:398-411`).

**Thread-safety.** It is read/written from at least two RTOS contexts: the pinned `SpindleTask`
(`main.cpp:80-85` -> `timerCallback` -> `spindle->update()` + `leadscrew->update()`, which both
read and mutate `GlobalState`) and the `DisplayTask` / `loop()` on another core
(`main.cpp:219-220`). Synchronization is **only** ad-hoc `volatile` on a handful of fields
(`:64-66,76-77`); the enum/int state that `update()` mutates (`setMotionMode`,
`setThreadSyncState`) has no `volatile`, no atomics, and no lock. `volatile` is not a memory
model - this is a latent data race, currently "working" by luck of timing and single-word writes.

**Latent hazard.** `getTargetSpeedDistanceInPulses()`/`update()` dereference and increment
`debugBuffer` whenever `getDebugMode()` is true, but `setDebugMode()` is **entirely commented
out** (`globalstate.cpp:18-48`), so `debugBuffer` is never allocated and debug mode can never be
turned on. The debug path is dead - but if anyone re-enables `setDebugMode` without restoring the
allocation, `debugBuffer` is a wild pointer written from the ISR task. Either restore it properly
or delete it.

**Direction.** Split into cohesive owners (`MotionState`, `UiState`, `OtaState`) and define an
explicit, minimal hand-off between the spindle task and the UI/loop - a lock-free
single-producer/single-consumer queue or documented `std::atomic` fields - rather than a shared
mutable singleton. Remove the public debug pointers from the state object entirely (inject a
debug sink if needed).

**Risk/effort:** large / high risk (touches concurrency and the hot path). High value for
correctness. No native coverage of the concurrency, so proceed with hardware validation.

### A9. WebSettings model (do NOT modify now)

`src/WebSettings.cpp` is **off-limits for edits** in this pass (it has uncommitted captive-portal
changes). Design notes only; **bugs are covered in `docs/websettings-review.md` and not repeated
here.**

**Current shape.** The settings page is built by hundreds of `html += "..."` concatenations with
macro helpers (`DEFAULTWEBSETTING`, `DEFAULTLATHESETTING`, `SETCONFIG` at
`WebSettings.cpp:18-25`). Persistence is raw struct blitting straight to flash via `ESP.flashRead`
/ `flashWrite` at fixed offsets (`WebSettings.cpp:14-16,123-131,158-160`), with validity keyed on
a single shared `CHECKVALUE` magic and **no schema/versioning**. Parsing uses throwing
`std::stoi` (`:25`).

**Why it matters (design).** The form layout, the POST-parse, and the persisted struct are three
parallel hand-maintained lists that must stay in lockstep; every new field is edited in three
places and one mismatch corrupts flash (no version tag means a reordered/added field reads stale
bytes back as "valid" - review Finding 5). The manual `html +=` wall is error-prone and mislabels
units (review Finding 6).

**Direction.** Drive everything from **one settings registry**: a table of field descriptors
(name, type, label/unit, min/max/default, offset or accessor). The same table renders the form,
parses/validates the POST (with non-throwing conversion and range clamping), and defines the
persisted layout. Add a struct version tag and a CRC over the payload instead of a bare magic;
erase all flash sectors the writes span. Templated field descriptors keep it type-safe with
little boilerplate.

**Risk/effort:** large. Defer until the captive-portal work lands and there is test coverage for
the settings round-trip (currently none).

---

## Part B - refactors actually applied (all verified, 18/18 green)

Every change below was applied and the suite re-run; `test_latheconfig`, `test_leadscrew`,
`test_smoke` stayed at **18/18 passing** and `test_regression` stayed failing **by design**.
Line endings were preserved (CRLF for the existing `lib/` sources, matching the repo).

1. **Extracted `positiveModulo` helper** - `lib/config/config.h:23-30`. New
   `inline int positiveModulo(int, int)` with identical integer semantics to `((x%m)+m)%m`.
2. **Applied it at every matching site:**
   - `lib/leadscrew/leadscrew.cpp:59`, `:68` (was `:60`,`:69`) - sync-position wrap.
   - `lib/leadscrew/leadscrew.cpp:298` (was `:299`) - expected-sync-position wrap.
   - `lib/spindle/TestSpindle.cpp:38` - native double's position wrap.
   - `ESPSpindle.cpp:49` deliberately **left unchanged** (different `(pos+ppr)%ppr` semantics,
     not covered by native tests - unifying it would be a behavior change / bug fix).
3. **Removed dead `//m_accumulator` comments** - `lib/leadscrew/leadscrew.h:67` and
   `lib/leadscrew/leadscrew.cpp:27`.
4. **Deleted stale `lib/spindle/TestSpindle.h`** - obsolete, unreferenced, non-compiling.

### Top proposals deliberately NOT applied

- **Fix the dropped `/ leadscrewPitchMm` divisor** and the other review bugs - owner chose
  report-only; `test_regression` pins the discrepancy on purpose.
- **Delete the Teensy envs / `leadscrew_io_teensy.h` and collapse `ELS_USE_RMT`** (A4) - safe in
  principle but outside the "provable by passing tests" envelope (the non-RMT branch has no
  coverage; the Teensy env is not built by the native suite).
- **Collapse `LatheConfigDerived` pass-throughs / precompute derived values** (A5) - a real
  improvement but a larger, wiring-touching change better done deliberately.
- **Introduce the spindle interface** (A6) and the **`Leadscrew` / `GlobalState`
  decompositions** (A8, A7) - architectural; require broader coverage first.
- **Any WebSettings change** (A9) - file is off-limits this pass.

### Important operational finding (see A3)

While applying Part B I confirmed the **committed** `HEAD:lib/spindle/TestSpindle.cpp` is the
old broken double and that the working test harness (`test/`, `scripts/`, the good
`TestSpindle.cpp`) is **entirely uncommitted**. The good double was momentarily lost to a stray
`git checkout` during this task and was **reconstructed verbatim** (from its read-back content)
plus the one-line `positiveModulo` change, then re-verified at 18/18. Committing the harness and
replacing the stale double should be the very next step - it is currently the single biggest risk
to the test safety net.
