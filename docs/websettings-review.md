# WebSettings Commits — Code Review

Report-only review. No source files were modified. Scope: commits `b0b6137` ("Made all lathe
properties configurable via web") and `5db03d7` ("Improved websetting experience") against parent
`cbd1b22`, plus the uncommitted working-tree captive-portal change to `src/WebSettings.cpp`.

> **Update — Finding 1 (the steps-per-mm regression) has since been FIXED.** The `/ leadscrewPitchMm`
> divisor was restored in `LatheConfigDerived::leadscrewStepsPerMm()`; `test_latheconfig` now pins the
> corrected (pre-websettings) scaling and the `test_regression` suite that documented the defect has
> been retired. Findings 2 onward remain open. The analysis below is preserved as the original review.

## Executive summary

**Runtime behaviour is NOT fully preserved.** Web-configurability was added correctly for most
settings, but one confirmed regression changes motion behaviour at the default configuration:

**Headline regression:** `LatheConfigDerived::leadscrewStepsPerMm()`
(`lib/config/latheconfig.cpp:38-40`) dropped the `/ leadscrewPitchMm` divisor that the old
`ELS_LEADSCREW_STEPS_PER_MM` macro had. At the default config this makes steps-per-mm **800 instead
of 314.96 (2.54x too high)**. Everything derived from it is inflated by the same 2.54x factor:
acceleration, jog speed limit, and max speed limit are all ~2.54x too aggressive, and the initial
pulse delay is 2.54x too short. Thread/feed **pitch tracking is unaffected** (see Finding 1). The
machine accelerates harder and caps speed higher than the pre-websettings firmware. Confirmed by the
`test_regression` suite (appendix).

The direction-mapping change preserves the old default behaviour and correctly adds a working invert
option. The remaining findings are robustness / forward-compatibility concerns and cosmetic issues.
The captive-portal working-tree change is functionally correct with only minor notes.

## Findings (ranked by severity)

| # | Severity | Location | Description | Evidence | Recommended fix (DO NOT APPLY) |
|---|----------|----------|-------------|----------|-------------------------------|
| 1 | **High** | `lib/config/latheconfig.cpp:38-40` | `leadscrewStepsPerMm()` returns `stepperPpr * (num/den)` = 800; the old macro divided by `leadscrewPitchMm`, giving 314.96. Inflates `accellerationPulseSec()`, `leadscrewMaxSpeedPps()`, `jogSpeedPps()` (all mm-based derived values) and shrinks `leadscrewInitialPulseDelay()` by 2.54x at defaults. | `test_regression` (appendix): stepsPerMm 800 vs 314.96; accel 120000 vs 47244; jog/max 32000 vs 12598.4; initial pulse delay 2500 vs 6350us. Old macro: `git show cbd1b22:lib/config/config.h:42-43`. Consumed at `lib/leadscrew/leadscrew.cpp:373,380,391` and `src/main.cpp:150-153`. | Restore divisor: `return stepperPpr()*(num/den)/leadscrewPitchMm();`. |
| 2 | Medium | `src/WebSettings.cpp:25,146-153` | `SETCONFIG` macro uses `std::stoi(arg.c_str())` for every integer field. `std::stoi` **throws** `std::invalid_argument` on empty/non-numeric input (e.g. a `type=number` field left blank in the POST). An uncaught throw on ESP32 → `std::terminate`/abort → reboot. This is a new robustness regression: before `b0b6137`, `setValues` only `strcpy`'d string fields and parsed nothing. | `SETCONFIG` expands to `std::stoi(webServer->arg(...).c_str())` with no try/catch or validation; called for 8 fields at lines 146-153. | Validate/`strtol` with default fallback, or wrap parsing in try/catch, or use `arg.toInt()`. |
| 3 | Medium | `lib/spindle/ESPSpindle.cpp:48`; `latheconfig.cpp:39` | Newly web-settable fields feed divisors with no zero-guard. `Spindle::setCurrentPosition` does `(position+ppr)%ppr` with `ppr=config->spindleEncoderPpr()`; a POST of `spindleEncoderPpr=0` → divide-by-zero crash. Likewise `gearboxRatioDenominator=0` in `leadscrewStepsPerMm()`/`gearboxRatio()`. Previously these were compile-time constants and unreachable. | `ESPSpindle.cpp:47-49`; `latheconfig.cpp:39,59`. Values come straight from the form via Finding 2's `SETCONFIG`. | Clamp to a minimum of 1 on load, or reject invalid values in `setValues`. |
| 4 | Low | `src/WebSettings.cpp:152,155` | Redundant parse: `SETCONFIG("leadscrewPitchMm", config.leadscrewPitchMm)` runs `std::stoi` on the float field, immediately overwritten by `std::stof` at line 155. Harmless to the value (`stoi("2.540000")`→2 without throwing), but confusing and still shares Finding 2's throw-on-empty risk. | Lines 152 then 155 both assign `config.leadscrewPitchMm`. | Drop the `SETCONFIG` line for the pitch field; keep only the `stof`. |
| 5 | Low | `WebSettings.h:8-13`; `latheconfig.h:6,9`; `WebSettings.cpp:14-16,158-160` | Flash layout is currently correct but fragile. Computed: `sizeof(WebSettings)=612`, `sizeof(LatheConfig)=40`; `address=0x3000` (12288), `latheaddress=12900`. Erase targets sector `(0x9000+0x3000)/4096 = 12` → bytes [49152,53248). WebSettings occupies [49152,49764), LatheConfig [49764,49804) — **both inside the single erased sector, so no spill**. But the erase erases exactly one sector sized off WebSettings' address; if `url`/fields grow past ~3.4KB, LatheConfig would spill into an un-erased next sector and silently fail to persist. Also both structs share one `CHECKVALUE` with no version field, and validity relies on exact struct layout/padding — a future field add/reorder makes stale flash read back as "valid". | Address math verified by compilation (g++). `check`/`CHECKVALUE` scheme at `WebSettings.cpp:18-19,143,156` and `main.cpp:~135`. | Erase all sectors spanned by both writes; add a struct/version tag; consider a CRC over the payload instead of a fixed magic. |
| 6 | Low (cosmetic) | `src/WebSettings.cpp:97,102,107` | UI labels jog speed and max speed as "m/s" and acceleration as "m/s^2", but the values are mm/s and mm/s^2 (struct comments `latheconfig.h:17-19` confirm mm). Misleads the operator by 1000x in labelling only; stored values are unaffected. | Labels at lines 97/102/107 vs comments at `latheconfig.h:17-19`. | Relabel to mm/s and mm/s^2. |
| — | Info (OK) | `latheconfig.cpp:62-67`; `leadscrew.cpp:259,268` | **Direction mapping preserved at default.** Old `ELS_DIR_RIGHT=1 / ELS_DIR_LEFT=0` in both `#ifdef` branches (old macro was a no-op). New `dirRight()=invert?1:0`, `dirLeft()=invert?0:1` with default `invertDirection=true` → 1/0, matching old. The checkbox (`WebSettings.cpp:73-77`, checked when uninitialised or true) and the comment "true => right = 1" are consistent, and unchecking now genuinely swaps direction — a new, intended capability, not a regression. | — | None. |
| — | Info (OK) | `leadscrew.cpp:47`; `main.cpp:150-153` | **Thread/feed pitch tracking unaffected.** `m_ratio = pitch*motorPulsePerRevolution/(leadscrewPitch*encoderPPR)`; `motorPulsePerRevolution` passed as `stepperPpr*gearboxRatio=800` (matches old) and `m_ratio` never uses steps-per-mm. | test_regression only fails the speed/accel cases; ratio inputs unchanged. | None. |

### Other diffs reviewed (no behaviour change)

- `src/main.cpp`: statics → heap pointers (`new`); wiring identical otherwise. New behaviour: setup
  now enters AP/settings mode when `H2` held **OR** flash check invalid (`!checks`) — a sensible
  first-boot addition. In AP mode `leadscrew/spindle/keyArray/keyPad` stay null but are never
  dereferenced (tasks are created only in the else branch; `loop()` calls `wifiLoop()` when
  `configMode`). No null-deref path found.
- `src/keyarray.cpp/.h`: `keyArray` static → pointer; mechanical, consistent.
- `lib/display/ST7789_320_240displaylvgl.h`: `DRAW_BUF_SIZE` re-parenthesised (value identical, left-
  assoc); added no-arg `Display()` ctor used by AP mode.
- `lib/spindle/ESPSpindle.cpp`, `spindle.h`: macros → `config->spindleEncoderPpr()`; identical at
  default 1200 (but see Finding 3).

## Captive-portal review (uncommitted working-tree change)

Adds `DNSServer` wildcard, OS-probe redirect endpoints, `onNotFound`, and
`dnsServer.processNextRequest()` in `wifiLoop`. Overall correct and safe.

- **Existing routes intact.** `/`, `/set`, `/reset` are registered before the probe endpoints and
  `onNotFound`; explicit routes take precedence, so `showPage`/`setValues`/`reset` are unaffected
  (`WebSettings.cpp:180-197`).
- **`WiFi.softAPIP()` valid at start — OK.** `runWifiSettings` (`main.cpp:97-111`) calls
  `WiFi.softAP()` and prints the IP *before* `startWebServer()`, so `softAPIP()` (192.168.4.1) is
  valid when `dnsServer.start(...)` and `redirectToPortal()` use it.
- **No watchdog/blocking concern.** `dnsServer.processNextRequest()` and `webServer->handleClient()`
  are both non-blocking polls; `loop()` runs them each iteration. DNS on port 53 is UDP poll-only.
  No long blocking call added.
- **Apple CNA:** redirecting `/hotspot-detect.html` with a 302 (instead of returning the expected
  "Success" body) does reliably mark the network captive and pop the CNA on iOS/macOS; the client
  follows the redirect to the portal. Acceptable. (Some implementations prefer serving the portal
  HTML directly at 200 for that path; the 302 approach is fine.)
- **Minor notes (not blocking):**
  - If `dnsServer.start` fails (port busy) the return value is ignored — cosmetic only.
  - Redirects are HTTP only; hosts that probe over HTTPS won't be intercepted, but the standard OS
    captive probes above are HTTP, so detection still triggers.
  - `onNotFound` also 302s favicon/asset requests to the portal — harmless.

## Appendix — `test_regression` output (evidence for Finding 1)

Command: `pio.exe test -e native -f test_regression -v`. Suite fails **by design**, pinning the
pre-websettings golden values.

```
[ RUN      ] PreWebSettingsRegression.LeadscrewStepsPerMm_DroppedPitchDivisor
test_regression.cpp:67: Failure
  d.leadscrewStepsPerMm() evaluates to 800,
  kOldStepsPerMm evaluates to 314.96060180664062   (diff 485.04)
[ RUN      ] PreWebSettingsRegression.LeadscrewInitialPulseDelay
test_regression.cpp:74: Failure
  d.leadscrewInitialPulseDelay() evaluates to 2500,
  kOldInitialPulseDelayUs evaluates to 6350        (diff 3850)
[ RUN      ] PreWebSettingsRegression.JogSpeedPps
test_regression.cpp:81: Failure
  d.jogSpeedPps() evaluates to 32000,
  kOldJogSpeedPps evaluates to 12598.400390625     (diff 19401.6)
[ RUN      ] PreWebSettingsRegression.LeadscrewMaxSpeedPps
test_regression.cpp:88: Failure
  d.leadscrewMaxSpeedPps() evaluates to 32000,
  kOldMaxSpeedPps evaluates to 12598.400390625     (diff 19401.6)
[ RUN      ] PreWebSettingsRegression.AccellerationPulseSec
test_regression.cpp:95: Failure
  d.accellerationPulseSec() evaluates to 120000,
  kOldAccelPulseSec evaluates to 47244             (diff 72756)
[  FAILED  ] 5 tests
============= 6 test cases: 5 failed, 0 succeeded =============
```

(Suites `test_latheconfig`, `test_leadscrew`, `test_smoke` pin current behaviour and pass; only
`test_regression` fails, and by design.)

### Old-vs-new derived values at default config

| Derived value                | old (cbd1b22) | new (master) | ratio |
|------------------------------|---------------|--------------|-------|
| leadscrewStepsPerMm          | 314.96        | 800          | 2.54x |
| jogSpeedPps (40 mm/s)        | 12598.4       | 32000        | 2.54x |
| leadscrewMaxSpeedPps (40)    | 12598.4       | 32000        | 2.54x |
| accellerationPulseSec (150)  | 47244         | 120000       | 2.54x |
| leadscrewInitialPulseDelay   | 6350.0 us     | 2500.0 us    | 0.39x |
| dirRight / dirLeft (default) | 1 / 0         | 1 / 0        | OK    |
| gearboxRatio                 | 2             | 2            | OK    |
| m_ratio (pitch tracking)     | unchanged     | unchanged    | OK    |
