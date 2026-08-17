# CLAUDE.md — working notes for this repo

Guidance for Claude Code (and humans) working on **TeensyELS**, an Electronic Leadscrew (ELS)
controller. Despite the name, the **active target is an ESP32** (LilyGO T-Display / ESP32-WROOM board);
Teensy support was removed. See `README.md` for the product-level overview.

## Build, test, flash, publish

PlatformIO CLI. If `pio` isn't on `PATH`, use the venv binary: `~/.platformio/penv/Scripts/pio.exe`.

- **Host unit tests (do this for every logic change):** `pio test -e native` — googletest/gmock, default
  env. Suites live in `test/test_*/`; host stubs (a virtual-clock `Arduino.h`, empty `ESP32Encoder`) are in
  `test/stubs/`; `scripts/exclude_espspindle_native.py` keeps `TestSpindle.cpp` the only `Spindle::` def on
  host. As of this writing the suite is ~54 cases and should stay green.
- **Build / flash the device:** `pio run -e esp32dev_usb -t upload`. The USB serial port is set in
  `platformio.ini` (`esp32dev_usb` `upload_port`). `pio run -e esp32dev_usb` (no `-t upload`) is a
  compile-check — do this after any change to device code.
- **Publish OTA:** `pio run -e esp32dev_publish -t upload` runs `scripts/release.sh` (see OTA below).
- `native` builds must NOT pull in ESP-only libs (lvgl / ESPAsyncWebServer / WiFi); guard esp32-only
  includes so host tests keep compiling.

## Architecture & the realtime constraint (read before touching motion code)

Two FreeRTOS tasks, pinned to different cores (see `src/main.cpp`):
- **SpindleTask** (core 0, high priority, small 4 KB stack): the hot loop. Each iteration runs
  `timerCallback()` → `spindle->update()` + `leadscrew->update()` (or `commsManager.loop()` during OTA).
- **DisplayTask** (core 1): buttons (`keyPad->handle()`) + LVGL `display->update()`.

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
`https://github.com/martinlong1978/TeensyELS/releases/latest/download/elstft.bin` (repo/asset constants in
`board.h`). The device checks `api.github.com/.../releases/latest` `tag_name` against `FIRMWARE_VERSION`
(`include/version.h`) and shows "No update available" if equal (see `ESPCommsManager`).

- OTA runs on its **own 24 KB task** (spawned from `ESPCommsManager::loop()`), NOT the 4 KB SpindleTask —
  TLS + CA-bundle + `Update` overflow 4 KB. Never move it back onto the SpindleTask.
- TLS uses the arduino-esp32 CA cert bundle via `setCACertBundle(rootca_crt_bundle_start)`. The extern
  symbol is **core-version specific** (`_binary_x509_crt_bundle_start` for framework-arduinoespressif32
  2.0.17); re-verify with `nm` if the core is upgraded. Download follows redirects
  (`HTTPC_STRICT_FOLLOW_REDIRECTS`) for the github.com → `*.githubusercontent.com` hop.
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

- Migrate the real lathe to GitHub OTA (it's seeded at the old home URL for a one-time pull).
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
