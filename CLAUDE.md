# CLAUDE.md — working notes for this repo

Guidance for Claude Code (and humans) working on **HalfNutELS**, an Electronic Leadscrew (ELS)
controller. The **target is an ESP32** (LilyGO T-Display / ESP32-WROOM board). The project was called
*TeensyELS* until Aug 2026, after the Teensy it began on; Teensy support was removed long before the
rename. The KiCad projects and board silkscreen still carry the old name. See `README.md` for the
product-level overview.

## Build, test, flash, publish

PlatformIO CLI. If `pio` isn't on `PATH`, use the venv binary: `~/.platformio/penv/Scripts/pio.exe`.

- **Host unit tests (do this for every logic change):** `pio test -e native` — googletest/gmock, default
  env. Suites live in `test/test_*/`; host stubs (a virtual-clock `Arduino.h`, empty `ESP32Encoder`) are in
  `test/stubs/`; `scripts/exclude_espspindle_native.py` keeps `TestSpindle.cpp` the only `Spindle::` def on
  host. As of this writing the suite is 455 cases and should stay green.
- **Build / flash the device:** `pio run -e esp32dev_usb -t upload`. The USB serial port is set in
  `platformio.ini` (`esp32dev_usb` `upload_port`). `pio run -e esp32dev_usb` (no `-t upload`) is a
  compile-check — do this after any change to device code.
- **Publish OTA:** `pio run -e esp32dev_publish -t upload` runs `scripts/release.sh` (see OTA below).
- `native` builds must NOT pull in ESP-only libs (lvgl / ESPAsyncWebServer / WiFi); guard esp32-only
  includes so host tests keep compiling.

## Architecture & the realtime constraint (read before touching motion code)

**Core 1 belongs to the spindle loop alone.** Everything else — display, keypad scan, OTA, the
capture uploader — is on core 0, where the SDK already puts WiFi, lwIP and the timer service.
Measured: the hot loop went from ~77,800 Hz to ~115,900 Hz when it stopped sharing (Aug 2026).

- **SpindleTask** (core 1, priority 24, small 4 KB stack): the hot loop. Each iteration runs
  `timerCallback()` → `spindle->update()` + `leadscrew->update()` (or `commsManager.loop()` during OTA).
- **DisplayTask** (core 0, priority 1): buttons (`keyPad->handle()`) + LVGL `display->update()`.
- **KeyScan** (core 0, priority 2): the 2 ms keypad matrix poll (`src/keyarray.cpp`).

**THE SPINDLE TASK MUST BE CREATED LAST.** `setup()` runs on the Arduino `loopTask`, which is itself
pinned to core 1 at priority 1. Creating the priority-24 spindle task on core 1 preempts it there and
then — permanently, because that loop never blocks — so *every statement after the creation call never
executes*. Created first, it silently ate the `DisplayTask` creation two lines below and the whole
watchdog setup; the symptom was "the display never initialises" and it caused an earlier attempt at
this core split to be reverted (`d7a9a7b` → `f66472d`). Anything that must happen at startup goes
ABOVE that call.

**Flash writes need the other core to answer.** A flash operation stops the opposite core and waits
for it to acknowledge a cache-disable. That core is now core 1, which never blocks — so code that
writes flash while the spindle runs must LOWER the spindle task's priority, never suspend it
(`src/DebugSink.cpp`). Suspending it deadlocks the device silently: `disableLoopWDT()` plus the two
`esp_task_wdt_delete()` calls mean nothing is watching.

**Keep `Leadscrew::update()` and the spindle path fast and inline.** No new virtual dispatch, heap
allocation, locks, or non-inlinable calls in that path. A lot of code there is inline on purpose. Derived
config values are precomputed once (`LatheConfigDerived`) specifically to keep divides out of the loop.

## Cross-task state — `GlobalState` (lib/global_state)

Singleton used as the coordination bus between the two tasks. Scalars read/written from both cores are
`volatile` (32-bit aligned → atomic on ESP32; `volatile` stops stale caching). **No locks** — do not add a
mutex on the hot path. If you add cross-task shared state, make it `volatile` too. The display reads OTA
progress/status here (`hasOTA`, `OTABytes/OTALength`, `GlobalOtaStatus`); the OTA task never calls display
methods directly.

## Config system (lib/config)

- `LatheConfig` (POD, defaults in `latheconfig.h`) + `LatheConfigDerived` (precomputes stepsPerMm, jog/max/
  accel pps, pulse delay, gearbox ratio, dir pins **once in the constructor** — config is fixed at runtime).
- **`CHECKVALUE`** is a validity sentinel stored with settings. Bumping it in `latheconfig.h` **invalidates
  all stored settings** so the device boots into first-run AP setup and re-adopts defaults. Use it when the
  default config/URL must be forced onto existing devices — it wipes saved WiFi + lathe params, so the user
  must reconfigure.
- Settings persist as a raw struct blit in the **NVS flash region** (`ESP.flashWrite` at `0x9000+0x3000`),
  NOT a filesystem — there is no fs partition (`my_4MB.csv`), so `uploadfs` is not applicable.

## Testing conventions

- Characterization/pinning style: observe current behaviour, assert it exactly. When adding tests that
  reveal a real bug, assert the CORRECT behaviour (let it fail), then fix — don't pin the broken behaviour.
- Thread/motion tests drive the spindle over the virtual clock (`setMockMicros`/`advanceMockMicros`) at a
  chosen PPS; `TestSpindle` reports a realistic time-based velocity so the leadscrew feed-forward is
  exercised. See `test/test_thread_sync/` (helix phase across speeds, endstop landing) and
  `test/test_settings/` (per-setting propagation + isolation).
- Constructors must initialise **all** members. These objects are heap-allocated (`new` in `main.cpp`), and
  heap isn't zero-initialised — relying on implicit zeroing has caused real bugs (dead buttons from an
  uninitialised `keycodeMillis`; garbage sync state). Watch for this whenever adding members.

## OTA / firmware releases

OTA pulls from **GitHub Releases** via the stable permalink
`https://github.com/martinlong1978/HalfNutELS/releases/latest/download/elstft.bin` (repo/asset constants in
`board.h`). The device checks `api.github.com/.../releases/latest` `tag_name` against `FIRMWARE_VERSION`
(`include/version.h`) and shows "No update available" if equal (see `ESPCommsManager`).

- OTA runs on its **own 24 KB task** (spawned from `ESPCommsManager::loop()`), NOT the 4 KB SpindleTask —
  TLS + CA-bundle + `Update` overflow 4 KB. Never move it back onto the SpindleTask.
- TLS uses the arduino-esp32 CA cert bundle via `setCACertBundle(rootca_crt_bundle_start)`. The extern
  symbol is **core-version specific** (`_binary_x509_crt_bundle_start` for framework-arduinoespressif32
  2.0.17); re-verify with `nm` if the core is upgraded. Download follows redirects
  (`HTTPC_STRICT_FOLLOW_REDIRECTS`) for the github.com → `*.githubusercontent.com` hop.
- **Devices flashed before the rename hold the old `TeensyELS` URL in NVS** — the update URL is a
  persisted web setting, and only the *default* moved to `HalfNutELS`. They keep updating because GitHub
  redirects a renamed repo, release-asset downloads included. So never create a new repo at
  `martinlong1978/TeensyELS`: that kills the redirect and bricks OTA for every device still on the old URL.
  Clearing settings (`CHECKVALUE`) or a fresh setup adopts the new default.
- **WiFi modem sleep must be OFF for the download** (`WiFi.setSleep(false)` in `wifiConnect()`). It is ON
  by default for `WIFI_STA`, and it is the difference between a 13-second update and one that never
  finishes. Measured, both directions: with sleep on, the transfer took a single 1364-byte MSS every
  ~350 ms — a beacon interval, not congestion — giving 2-4 kB/s and a hard stall around 18%; with it
  off, 1.56 MB lands in 13-25 s at 63-124 kB/s. Do not "restore" the default to save power: the OTA
  path ends in `ESP.restart()` regardless, so the radio is only awake for the duration.
- Things that instrumenting **ruled out**, so nobody re-litigates them: it is not the flash write (1% of
  elapsed time with sleep on, ~5.2 s of any run — that part is constant), not the SpindleTask holding
  core 1 against the cache-disable IPC (it parks correctly in `commsManager.loop()`'s `vTaskDelay`,
  measured at ~4 entries/sec), not heap (steady ~55 kB free throughout), and not the display (parking
  `DisplayTask` for a whole download made throughput *worse*, not better). The rename's extra redirect
  hop is real but costs one handshake, not throughput.
- RSSI is worth reading off the `OTA:` log line when an update is slow — it swung -86 to -49 dBm across
  runs on the same bench, and at the bottom of that range TLS connect itself intermittently fails.
- **To release:** bump `FIRMWARE_VERSION` in `include/version.h`, `pio run -e esp32dev_publish`, then
  `bash scripts/release.sh` (needs `gh` + `gh auth login`). It uploads `firmware.bin` as asset `elstft.bin`.
  Publish full releases (not drafts/pre-releases) or the `/latest/` permalink won't resolve.

## Display (lib/display)

- 320×240 landscape ST7789 via LVGL. `drawMode`/`drawPitch`/etc. render from `GlobalState`.
- **Colour order:** the panel is R↔B swapped and every colour is authored **pre-swapped** to compensate
  (e.g. red is `0x0000FF`). This renders correctly — it is NOT a bug. Author any new colour in the same
  swapped form, or take it from the palette. On the `ux-redesign` branch the `COLOUR_*` defines become a
  `DisplayPalette` struct with dark and light instances; the swap convention is unchanged.
  (Corrects an older note here: the negative-RPM text was described as rendering blue from an un-swapped
  `0xFF0000`. It never rendered at all — the branch tested `abs(rpm) < 0`, which is always false, so it was
  dead code. Fixed on `ux-redesign`: it now tests the signed value and uses the palette's fault colour.)
- LVGL QR widget (`LV_USE_QRCODE`) is enabled; the setup screen shows a Wi-Fi-join QR. There is no image
  flip API in this LVGL — the left-hand thread icon is a pre-flipped generated asset
  (`icons/threadSymbolReverse.c`).
- **Boot splash** (`Display::showSplash()`): brand mark + "HalfNut ELS" + `FIRMWARE_VERSION`, held for
  `SPLASH_HOLD_MS` (2 s) by **main.cpp**, not by the display library — `showSplash()` draws and returns,
  so the screenshot harness renders the same screen without waiting. It goes ABOVE `display->init()`,
  which opens with `lv_obj_clean()` and is therefore what clears it. Blocking there is safe *because*
  the SpindleTask does not exist yet; do not move the call below the task creation (see the realtime
  section). Normal boot path only — the AP-setup path wants credentials on screen fast.
- The mark is generated by **`scripts/make_logo.py`** (a sibling of `make_glyphs.py`, importing its
  shape primitives) → `icons/halfNutLogo.c`, a 96×96 **A8 alpha mask** recoloured from the palette like
  the mode glyphs, so it carries no colour and the R↔B swap does not apply to it. Edit the script and
  re-run it; never hand-edit the `.c`. It is a hex nut split across its centreline with the halves stood
  apart, and a **smooth** bore. Do not put thread crests back in that bore: the mark is a half-nut pair
  seen from the SIDE, where the thread is not visible, and at 96 px the crests also read as gear teeth —
  the wrong signal entirely for a device that deletes a lathe's change gears. The script's docstring
  carries the full reasoning.

### Look at the screens: `tools/screenshot`

**`bash tools/screenshot/render.sh`** renders the real `Display` class on the host and writes one 320×240
PNG per scenario into `tools/screenshot/out/`. Do this after ANY layout, palette or string change — the
`static_assert` block in `ST7789_320_240displaylvgl.cpp` checks box arithmetic, not legibility, and a
screen can pass all thirty of them while being unreadable.

- Needs `gcc`/`g++` on `PATH` (MSYS2 UCRT64 on this box: `PATH=/c/msys64/ucrt64/bin:$PATH`) and LVGL
  already fetched into `.pio/libdeps/esp32dev_*` — the SAME checkout the firmware links, built against the
  project's real `include/lv_conf.h`, so fonts/widgets/renderer limits match the device exactly.
- Only `Arduino.h`, `SPI.h`, `TFT_eSPI.h` and `lv_tft_espi_create()` are shimmed (`shim/`); the display,
  leadscrew, DRO, UiState and GlobalState code is the production code. Scenes drive it through the same
  public inputs the firmware does — including `UiState::handleKey()` for focus, so a screen the keypad
  cannot reach cannot be screenshotted either.
- `render.sh <scene>` renders one; `build/elsshot.exe --list` names them all. One scene per process
  (`lv_init()` is global).
- Each line of output is the proof the image is real: `colours=`, `ink=` (fraction differing from the
  modal colour) and `unpainted=`, the count of pixels still holding the pre-render sentinel. **`unpainted`
  must be 0** — anything else is a hole in the render and the script exits non-zero.
- PNGs are written R↔B **un**-swapped, i.e. as the panel displays them, so what you see is what the
  operator sees. Do not "correct" a colour that looks right in the PNG.

## Input: the keypad is POLLED (lib/keyscan + src/keyarray.cpp)

No interrupts. A 2 ms task scans the matrix and hands the raw code to `KeyScanner`, which debounces
by integration (a reading must persist 8 ms) and produces the gesture events `UiState` is built on:
`Press → Click → Release`, or `Press → Hold → Release` with **no Click after a Hold**. Pure C++,
host-tested (`test/test_keyscan`).

Do NOT reintroduce edge interrupts. The previous scheme armed RISING for a press and FALLING for a
release, re-armed as a side effect of each scan, behind a 10 ms lockout shared between the two — so a
release inside that window returned early *before* the re-arming line and left the pad waiting for an
edge that could never come. The keypad went dead until the 1 s hold timer rescanned. It also called
`pinMode()` and `attachInterrupt()` from inside the ISR, from flash, with no `IRAM_ATTR`. Full
post-mortem in `docs/keypad-audit.md`.

`bounceRejects()` / `ringDrops()` exist so the debounce threshold can be judged from the machine
rather than assumed. Both should stay at zero in normal use.

## Motion gotchas that cost a whole evening (Aug 2026)

- **`rmtWrite`'s third argument is an ITEM COUNT, not a byte count.** `sizeof(rmt_data)` made it 96,
  which both read 72 items past the end of a 24-element array AND exceeded the channel's `RMT_MEM_64`
  block, so `rmt_write_items` blocked while the hardware drained uninitialised heap. The spindle loop
  fell from 78 kHz to **58 Hz** while jogging. Only element `[0]` had ever been initialised, so what
  got transmitted was whatever heap followed the array — which **reshuffles on every rebuild**. That
  is why the same commit measured fast on one build and slow on the next.
- **An intermittent fault cannot be bisected on single trials.** The above sent a bisect across four
  commits chasing noise and produced three confident, wrong conclusions. What settled it was putting a
  counter on the loop and reading the number. Prefer measuring to reading diffs.
- **`ESP32Encoder` returns spurious ±32k deltas.** It runs the PCNT counter to `±INT16` and
  accumulates the wrap in a limit ISR; a `getAndClearCount()` racing that ISR returns a value at the
  16-bit boundary. Captured mid-cut: `+32765` then `−32766` in consecutive samples, which
  `Leadscrew::update()` multiplied by the ratio and saturated `posError` to −2³¹. **This was the
  original "180° forward jump while threading".** `Spindle::update()` now drops deltas beyond half the
  16-bit range. The existing spindle-angle modulo could not help: `setCurrentPosition()` takes its
  delta from the raw pre-modulo value, so the angle is corrected while the poisoned delta goes
  straight into `m_unconsumedPosition`.

## Gotchas

- PlatformIO / the IDE occasionally rewrite `.vscode/extensions.json` (and sometimes source files) with
  CRLF churn — revert those before committing so diffs stay clean. `lib/` sources are CRLF; `src/`/README are
  LF (mixed; a `.gitattributes` would settle it).
- `publish.cmd` is gitignored (a local wrapper); the real publish path is `scripts/release.sh` via the
  `esp32dev_publish` `upload_command`.
- The WiFi **captive portal** (config AP) serves the config page at any URL; on Android the browser routes
  over cellular when the AP is "captive", so the reliable path is the OS "Sign in to network" sheet, not the
  in-page browser QR.

## Known open items (optional)

- ~~Migrate the real lathe to GitHub OTA~~ — done; the lathe pulls from GitHub Releases.
- ~~Negative-RPM colour line (renders blue)~~ — fixed on `ux-redesign` and confirmed red in
  `tools/screenshot/out/rest-reverse-spindle.png`.

### Found by the first screenshot run (`ux-redesign`, unfixed)

All four are legibility, not layout, so no `static_assert` can catch them:

1. **Dark palette: `colourDisabled` (`0xCCCCCC`) is invisible on `background` (`0xF5F5F5`)** — 1.5:1.
   Kills all four band rules, the un-synced `SYNC` chip, and the `IDLE` state word + dot. The rest screen
   at idle has no readable machine state at all.
2. **Light palette: `textDim` (`0x757575`) on `colourDisabled` (`0x6B7280`) is invisible** — ~1.1:1,
   measured off the pixels. The unselected MODE tile labels ("FEED", "THREAD L") and a blocked menu card's
   name render as blank grey slabs. See `light-overlay-mode.png` / `light-menu-sync-blocked.png`.
3. **The pitch ticker's track is not the colour the code asks for.** `init()` sets `bg_color` on the
   slider's `LV_PART_MAIN` but never `bg_opa`, so it keeps the default theme's ~20% translucent main while
   the INDICATOR is opaque `colourDisabled`. The two therefore differ, and the ticker reads as a fill/
   progress bar — the exact thing the comment there says it must not do. Glaring in the light palette.
4. **The OTA and Wi-Fi screens are not pre-swapped.** They use LVGL's stock theme colours, so the update
   progress bar renders **orange** (`#2196F3` → `#F39621`) instead of blue. Also `"Checking for updates..."`
   at Montserrat 26 spans essentially the full 320 px with no margin.
