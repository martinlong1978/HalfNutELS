# ESP32 ELS — Electronic Leadscrew

An open-source **Electronic Leadscrew (ELS)** controller for a metalworking lathe, running on an
**ESP32-WROOM-32E**, with a 320×240 LVGL TFT interface, a 3×3 keypad and a rotary encoder.

An electronic leadscrew replaces a lathe's mechanical change gears / gearbox. Instead of a train of
gears mechanically coupling the spindle to the leadscrew, an **encoder** on the spindle is read by
the microcontroller, which drives a **stepper motor** on the leadscrew in software-defined ratio to
the spindle. This lets you select any thread pitch or feed rate electronically — for single-point
threading and powered feeding — without swapping gears.

> **Note on the name:** the repository is historically called *TeensyELS* because it began life on a
> Teensy microcontroller. Teensy support has since been **removed** — the active firmware targets the
> **ESP32**. See [Attribution](#attribution).

> **Note on the hardware:** the current design is the **LVGL board** in
> [`kicad/LVGL/`](kicad/LVGL/) (`TeensyELS v0.6`) — an ESP32-WROOM-32E soldered directly to the board,
> driving a 320×240 SPI TFT through LVGL. The older **2-layer** and **4-layer** boards, which socketed
> a LilyGO T-Display module (and carried a Teensy 4.1 footprint), are **legacy** and are kept only for
> reference. See [Legacy boards](#legacy-boards-2-layer--4-layer).

---

## Contents
- [Features](#features)
- [Hardware / Components](#hardware--components)
- [Legacy boards (2-layer / 4-layer)](#legacy-boards-2-layer--4-layer)
- [Getting boards made (PCB)](#getting-boards-made-pcb)
- [Building & flashing the firmware](#building--flashing-the-firmware)
- [Connecting & configuring](#connecting--configuring)
- [Operating the ELS — controls & display](#operating-the-els--controls--display)
- [Settings reference](#settings-reference)
- [Testing / development](#testing--development)
- [License](#license)
- [Attribution](#attribution)
- [Safety disclaimer](#safety-disclaimer)

---

## Features

- **Threading** — synchronise the leadscrew to the spindle at a selectable thread pitch for
  single-point threading. Metric (mm/rev) and imperial (TPI) thread pitch tables are built in.
- **Reverse threading** — a thread mode that runs the carriage in the opposite direction (from the
  left stop toward the right) for left-hand threads or threading away from a shoulder.
- **Feeding** — powered feed at a selectable feed rate (mm/rev metric, or thou/rev imperial).
- **Feed / thread mode switching** and **metric / imperial** switching from the keypad.
- **Jog** — move the carriage left/right under power. Supports both an interactive "hold to jog"
  mode and a "jog to stop" mode.
- **End stops / stop positions** — set left and right stop positions so the carriage automatically
  decelerates and stops at a repeatable point (useful for threading up to a shoulder).
- **Thread sync** — the leadscrew tracks the spindle's angular position so a partially cut thread can
  be picked up again on the next pass.
- **Button lock** — lock the keypad to prevent accidental changes while cutting.
- **Web-based configuration** — all lathe parameters (encoder PPR, stepper PPR, gearbox ratio,
  leadscrew pitch, speeds, acceleration) plus WiFi credentials are configured over a built-in web
  page; settings are stored in the ESP32's non-volatile flash.
- **Over-the-air (OTA) firmware update** — the device can pull a new firmware image from a configured
  HTTP(S) URL.
- **On-device display** — a 240×320 ST7789 SPI TFT, driven in landscape (**320×240**) via **LVGL**,
  shows current mode, pitch, spindle RPM and status.
- **Rotary encoder UI** — an illuminated (RGB) rotary encoder selects the pitch / jog rate; its
  built-in LED shows the run state at a glance (the firmware drives the red and green channels,
  alternating them to flash a two-colour status).

---

## Hardware / Components

The controller is built around a custom PCB. KiCad design files, gerbers and fabrication outputs are
in the [`kicad/`](kicad/) directory.

**The current design is [`kicad/LVGL/`](kicad/LVGL/)** — silkscreened *TeensyELS v0.6*, a **4-layer**
board of roughly **113 × 109 mm**. Unlike the older boards it does not socket a dev-board module: the
**ESP32-WROOM-32E is soldered directly to the PCB**, the display is a separate SPI TFT module on a
14-pin socket, and the board is designed to be **SMT-assembled** (JLCPCB) with only the connectors and
the rotary encoder hand-soldered.

Everything below refers to the LVGL board. For the older boards see
[Legacy boards](#legacy-boards-2-layer--4-layer).

### Bill of materials

**SMT parts** — taken from the JLCPCB production BOM
([`kicad/LVGL/jlcpcb/production_files/BOM-TeensyELS.csv`](kicad/LVGL/jlcpcb/production_files/BOM-TeensyELS.csv)),
cross-checked against the schematic:

| Designator(s) | Qty | Part / Value | Footprint | LCSC |
|---|---|---|---|---|
| U2 | 1 | **ESP32-WROOM-32E** — main controller (Wi-Fi module, 4 MB flash) | ESP32-WROOM-32D | C701342 |
| U1 | 1 | AMS1117-3.3 — 5 V → 3.3 V regulator | SOT-223-3 | C6186 |
| Q2–Q7, Q9 | 7 | BSS138 N-channel MOSFET (3.3 V ↔ 5 V level shifting, LED drive) | SOT-23 | C7420339 |
| C1 | 1 | 10 µF capacitor | 0805 | C15850 |
| C2 | 1 | 22 µF capacitor | 0805 | C45783 |
| C3, C4 | 2 | 0.1 µF capacitor | 0805 | C49678 |
| R1–R8, R11, R12, R18, R20, R21 | 13 | 10 kΩ resistor (pull-ups) | 0805 | C17414 |
| R9, R10, R19 | 3 | 330 Ω resistor (encoder LED series) | 0805 | C17630 |
| R13, R14 | 2 | 0 Ω link (encoder direct-connect option) | 0805 | C17477 |
| R17 | 1 | 1 kΩ resistor | 0805 | C17513 |
| SW1–SW10 | 10 | Omron B3FS-101xP **SMD** tactile switch (SW2–SW10 = the 3×3 keypad; SW1 = stepper-enable override) | SW_SPST_Omron_B3FS-101xP | C231324 |
| J5 | 1 | 2×3 1.27 mm header — UART programming header | PinHeader 2x03 1.27 mm SMD | C42372553 |

**Not fitted (DNP)** — an alternative encoder input path, see the note below:

| Designator(s) | Qty | Part / Value |
|---|---|---|
| Q1, Q8 | 2 | BSS138 (encoder level-shift MOSFETs) |
| R15, R16 | 2 | 1 kΩ (gate resistors for Q1/Q8) |

**Through-hole / hand-soldered parts** — these are *not* in the assembly BOM and must be fitted
yourself:

| Designator | Qty | Part | Purpose |
|---|---|---|---|
| J1 | 1 | Shielded RJ45 jack (RCH RC01937) | **Stepper driver** — step / dir / enable / alarm, at 5 V |
| J2 | 1 | JST-XH 4-pin vertical header (B4B-XH-A) | **Spindle encoder** — 5 V, GND, A, B |
| J3 | 1 | 1×14 2.54 mm socket | **Display module** (see below) |
| J4 | 1 | 2-pin 5.08 mm screw terminal (CUI TB007-508-02) | **5 V power input** |
| E1 | 1 | Illuminated (RGB) rotary encoder, SparkFun footprint | Pitch / jog-rate selection + status LED |

Not on the board at all, but **required to build a working ELS**:

- A **240×320 SPI TFT module** with the common **14-pin 2.54 mm header** (9 display pins + 5 touch
  pins). The firmware drives it as an **ST7789** and uses it in landscape, 320×240. Only the display
  pins are wired — the 5 touch pins are unconnected, and the backlight pin is tied to +3.3 V, so the
  backlight is permanently on and is *not* under software control.
- A **spindle encoder** — a quadrature encoder on the lathe spindle, wired to **J2** (not J1). The
  default configuration assumes **1200 PPR** (see [Settings reference](#settings-reference)).
- A **stepper motor + driver** for the leadscrew, wired to the RJ45 jack **J1**. The board provides
  step, dir and enable as 5 V open-drain outputs (10 kΩ pull-ups to +5 V) and accepts the driver's
  alarm output. The default configuration assumes a **400 step/rev** stepper.
- A **5 V supply** into J4 (which also feeds the encoder and the 3.3 V regulator), plus a suitable
  **power supply for the stepper driver** itself.
- A **USB-to-serial adapter** for flashing — the board has no USB connector, see
  [Building & flashing](#building--flashing-the-firmware).

> **Encoder input — 3.3 V vs 5 V.** J2's A/B pins can reach the ESP32 two ways: directly through the
> 0 Ω links **R13/R14**, or level-shifted through **Q1/Q8 + R15/R16**. The assembly BOM fits the 0 Ω
> links and leaves the MOSFET path off, i.e. it expects an encoder whose outputs are safe to feed
> straight into a 3.3 V GPIO. If your encoder swings to 5 V, fit Q1/Q8 and R15/R16 and leave R13/R14
> off. **Please verify against the schematic** ([`kicad/LVGL/TeensyELS.kicad_sch`](kicad/LVGL/TeensyELS.kicad_sch))
> before ordering.

### Connectors

| Ref | Type | Pinout |
|---|---|---|
| **J4** | 2-pin screw terminal | 1 = GND, 2 = **+5 V in** |
| **J2** | JST-XH 4-pin | 1 = +5 V, 2 = GND, 3 = encoder B, 4 = encoder A |
| **J1** | RJ45 (shielded) | 1 = ALARM (in), 3 = ENABLE, 5 = DIR, 7 = STEP; 2/4/6/8 = GND; shield = GND. All 5 V. |
| **J3** | 1×14 socket (display) | 14 = VCC, 13 = GND, 12 = CS, 11 = RESET, 10 = DC, 9 = MOSI, 8 = SCK, 7 = LED (tied to +3.3 V), 6 = MISO (not connected to the ESP32); 5–1 = touch pins, unconnected |
| **J5** | 2×3 1.27 mm | 1 = EN, 2 = +3.3 V, 3 = TXD, 4 = GND, 5 = RXD, 6 = BOOT (GPIO0) |
| **SW1** | on-board push button | Pulls the stepper **ENABLE** line to +5 V — a manual override, independent of the firmware |

### Pin assignments (ESP32)

Defined in [`lib/config/board.h`](lib/config/board.h) and matching the LVGL schematic:

| Function | GPIO |
|---|---|
| Spindle encoder A / B (J2) | 35 / 34 |
| UI rotary encoder A / B (E1) | 39 / 36 |
| Encoder status LED — red / green / blue (E1) | 22 / 21 / 12 |
| Leadscrew step | 25 |
| Leadscrew direction | 26 |
| Stepper enable | 17 |
| Stepper driver alarm input | 27 *(wired on the board; not read by the current firmware)* |
| Button-array columns (H1/H2/H3) | 32 / 33 / 2 |
| Button-array rows (V1/V2/V3) | 13 / 14 / 15 |
| TFT display (MOSI/SCLK/CS/DC/RST) | 19 / 18 / 5 / 16 / 23 |
| TFT backlight (`TFT_BL`) | 4 *(defined in the build flags but **not connected** on this board)* |
| Programming UART TXD / RXD (J5) | 1 / 3 |

> The A/B pins of both encoders are swapped in `board.h` relative to the schematic net names — that
> only sets which way round the counting goes, and is deliberate.

---

## Legacy boards (2-layer / 4-layer)

Two earlier board revisions are kept in the repository for reference. **They are not the current
design** and the firmware is no longer developed against them:

| Directory | Silkscreen | Layers | Controller |
|---|---|---|---|
| [`kicad/2Layer/`](kicad/2Layer/) | TeensyELS v0.3 | 2 | LilyGO T-Display module socket + Teensy 4.1 footprint |
| [`kicad/4Layer/`](kicad/4Layer/) | TeensyELS v0.5 | 4 | LilyGO T-Display module socket + Teensy 4.1 footprint |

Notable differences from the LVGL board:

- The MCU was a **socketed LilyGO TTGO T-Display** module (ESP32 + built-in 1.14" 240×135 ST7789),
  with an unpopulated **Teensy 4.1** footprint alongside it from the project's Teensy origins. Teensy
  support has since been removed from the firmware entirely.
- Because the T-Display module carries its own USB port, no separate programming header was needed.
- **J3** was a 4-pin **I²C** header (SDA/SCL/5 V/GND) for an SSD1306 OLED, rather than a TFT socket.
- Through-hole (Omron B3F) tactile switches instead of SMD ones.
- The current firmware's display code targets a 320×240 LVGL screen, so it will not drive the
  T-Display's 240×135 panel as-is (`ELS_DISPLAY` in [`lib/config/board.h`](lib/config/board.h) still
  lists the older options, but they are not maintained).

J1 (stepper), J2 (encoder), J4 (5 V in) and E1 (rotary encoder) serve the same purposes as on the
LVGL board.

> The BOM and production files under `kicad/4Layer/production/` describe **that** board, not the
> current one.

---

## Getting boards made (PCB)

Fabrication outputs for the LVGL board are pre-generated under [`kicad/LVGL/`](kicad/LVGL/). The
JLCPCB set in [`kicad/LVGL/jlcpcb/production_files/`](kicad/LVGL/jlcpcb/production_files/) is the one
to use — it is the only output that carries **LCSC part numbers**, and its BOM matches the assembly
intent (SMT parts only, encoder level-shift path left off).

> ⚠️ **Do not use `kicad/LVGL/production/bom.csv` or `positions.csv`.** Those files are stale
> carry-overs from the 4-layer board — they still list a TTGO T-Display and a Teensy 4.1 and do not
> describe this board. Regenerate them from KiCad if you need them.

### Option A — JLCPCB (bare board, or with assembly)

1. Go to [jlcpcb.com](https://jlcpcb.com) and click **Add gerber file**.
2. Upload [`kicad/LVGL/jlcpcb/production_files/GERBER-TeensyELS.zip`](kicad/LVGL/jlcpcb/production_files/GERBER-TeensyELS.zip)
   (do **not** unzip it — upload the zip directly). The KiCad-native export
   [`kicad/LVGL/gerbers.zip`](kicad/LVGL/gerbers.zip) is equivalent if you prefer it.
3. Confirm the auto-detected settings: **Layers = 4**, and the board dimensions shown in the preview
   (≈113 × 109 mm). Choose thickness, colour, surface finish, etc. as desired.
4. *(Optional — assembly / PCBA)* Enable **PCB Assembly** (top side only), then upload:
   - **BOM:** [`kicad/LVGL/jlcpcb/production_files/BOM-TeensyELS.csv`](kicad/LVGL/jlcpcb/production_files/BOM-TeensyELS.csv)
   - **Pick-and-place / CPL:** [`kicad/LVGL/jlcpcb/production_files/CPL-TeensyELS.csv`](kicad/LVGL/jlcpcb/production_files/CPL-TeensyELS.csv)
5. Review the placement in JLCPCB's assembly preview — pay particular attention to the orientation of
   U1, U2 and the MOSFETs — resolve any part substitutions, and place the order.
6. Hand-solder the through-hole parts afterwards: **J1, J2, J3, J4 and the rotary encoder E1**.

### Option B — PCBWay

1. Go to [pcbway.com](https://www.pcbway.com) and choose **Quick-order PCB → Add Gerber File**.
2. Upload [`kicad/LVGL/gerbers.zip`](kicad/LVGL/gerbers.zip).
3. Confirm layer count (**4**) and dimensions, pick your board options, and add to cart.
4. *(Optional — assembly)* Choose **Turnkey Assembly** and provide the BOM and pick-and-place files
   above. PCBWay does not use LCSC numbers directly, so expect to confirm equivalents for each line.

> The legacy 2-layer and 4-layer boards can still be ordered from their own directories
> ([`kicad/2Layer/gerbers.zip`](kicad/2Layer/gerbers.zip), 2 layers;
> [`kicad/4Layer/gerbers.zip`](kicad/4Layer/gerbers.zip), 4 layers) — but the current firmware
> targets the LVGL board.

---

## Building & flashing the firmware

The firmware is a [PlatformIO](https://platformio.org/) project (see
[`platformio.ini`](platformio.ini)).

### 1. Install PlatformIO

Install the [PlatformIO IDE extension for VS Code](https://platformio.org/install/ide?install=vscode),
or the [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html).
PlatformIO will automatically download the ESP32 toolchain and the libraries listed in
`platformio.ini` on first build.

### 2. Connect a USB-to-serial adapter

The LVGL board has **no USB connector** — the ESP32-WROOM-32E is programmed through the 2×3 1.27 mm
header **J5**:

| J5 pin | Signal | Adapter |
|---|---|---|
| 1 | EN (reset) | RTS *(optional — for auto-reset)* |
| 2 | +3.3 V | — *(do not back-feed if the board is powered from J4)* |
| 3 | TXD (ESP32 → adapter) | RX |
| 4 | GND | GND |
| 5 | RXD (adapter → ESP32) | TX |
| 6 | BOOT (GPIO0) | DTR *(optional — for auto-bootloader)* |

Use a **3.3 V** adapter. If you only wire TX/RX/GND, you must put the module into the bootloader by
hand — hold GPIO0 low, pulse EN low and release it, then start the upload.

*(The legacy 2-layer/4-layer boards socket a LilyGO T-Display, which has its own USB port; on those,
just plug the module in.)*

### 3. Build & upload

There are two ESP32 build environments:

- **`esp32dev_usb`** — build and flash over the serial adapter. This is the environment to use for
  normal development. Edit the `upload_port` in `platformio.ini` (default `COM14`) to match the port
  your adapter enumerates as.

  ```sh
  pio run -e esp32dev_usb -t upload
  ```

- **`esp32dev_publish`** — a release build that uses a custom upload command (`publish.cmd`) for
  producing/publishing an OTA image rather than a direct flash.

  ```sh
  pio run -e esp32dev_publish
  ```

Both ESP32 environments still declare `board = lilygo-t-display`. That is just a convenient 4 MB
ESP32-D0WD profile and is compatible with the WROOM-32E module — the display geometry that matters is
set explicitly in the build flags (`TFT_WIDTH=240`, `TFT_HEIGHT=320`, ST7789 driver, SPI pins), and a
custom 4 MB partition table (`my_4MB.csv`) is used.

> The `teensy41` / `teensy41_debug` environments remain in `platformio.ini` for historical reasons but
> are **not the supported target** — the code paths for Teensy have been removed.

---

## Connecting & configuring

Configuration is done over WiFi through a built-in web page — there is no need to recompile to
retune the firmware for your lathe.

1. **Enter configuration mode.** The device starts a WiFi **Access Point** on first boot (when no
   valid settings are stored yet), or whenever you **hold the centre button of the keypad — Half Nut —
   while powering on**. See [`src/main.cpp`](src/main.cpp).
   - AP SSID: **`ELS_Wifi`**
   - AP password: **`123456789`**
2. **Connect** your phone or laptop to that access point. The SSID, password and IP address are shown
   on the device's display, along with a QR code for the WiFi credentials.
3. **Open the device in a browser.** The firmware runs a **captive portal** — a catch-all DNS server
   plus the usual Android/Apple/Windows detection URLs — so most phones will pop the configuration
   page up automatically on connecting. If yours doesn't, browse to the IP shown on the display
   (typically `192.168.4.1`) to reach the *ELS Setup* page.

4. **Fill in the settings** (see [Settings reference](#settings-reference)) and press **Submit**.
   Settings are written to the ESP32's flash. Press **Reset** on the page (or power-cycle) to reboot
   into normal operation with the new settings.

---

## Operating the ELS — controls & display

In normal operation the 320×240 display shows the current **feed/thread pitch**, the **mode**, spindle
**RPM**, the **end-stop** status, the sync state, and the keypad **lock** state. Input is via a
**3×3 button keypad** (scanned as a matrix on GPIO 32/33/2 × 13/14/15) plus the **illuminated rotary
encoder**.

> **The keypad starts LOCKED at power-on** as a safety measure — press **Lock** once to unlock before
> anything else will respond.

### Keypad layout

The nine keys of the matrix, as they sit on the board (SW2–SW10, bottom-left of the PCB; the rotary
encoder is above and to their right, with the display along the top):

| | Left | Centre | Right |
|---|---|---|---|
| **Top row** | Rate − | Rate + | Mode |
| **Middle row** | Thread Sync | Half Nut | Enable |
| **Bottom row** | Lock | Jog Left | Jog Right |

*(Holding **Half Nut** — the centre key — during power-on boots into WiFi configuration mode.)*

### Modes

Press **Mode** to cycle through:

| Mode | What it does |
|---|---|
| **Feed** | Leadscrew feeds at a set distance per spindle revolution (mm/rev or thou/rev). |
| **Thread** | Leadscrew is synced to the spindle to cut a thread; starts at the **right** stop and travels **left**. |
| **Thread ↺ (reverse)** | Same as Thread but the leadscrew travels the **opposite** way — starts at the **left** stop and moves **right** (for left-hand threads / threading away from the shoulder). Shown with a ↺ marker next to the pitch. |
| **Jog** | Move the carriage independently of the spindle; the encoder/rate buttons set the jog speed. |

*Hold* **Mode** to toggle between **metric** and **imperial** units.

### Button reference

| Control | Press (click) | Hold |
|---|---|---|
| **Mode** | Cycle Feed → Thread → Thread ↺ → Jog | Toggle metric / imperial |
| **Rate +** / **Rate −** | Select next / previous pitch (or jog speed in Jog mode) | — |
| **Rotary encoder** | Turn to change pitch / jog speed | — |
| **Enable** | Start / stop the leadscrew following the spindle (it decelerates to a stop, it does not stop dead) | — |
| **Lock** | Lock / unlock the keypad | — |
| **Jog Left** / **Jog Right** | *Feed/Thread:* jog the carriage to the set left/right stop (press again to stop early). *Jog mode:* jog while the button is held. | *Feed/Thread:* **set** the left/right stop at the current position, or **clear** it if already set |
| **Thread Sync** | — | Reset / re-initialise the display |
| **Half Nut** | Toggle debug mode | **Trigger an over-the-air firmware update** (see below) |

### Cutting a thread (typical flow)

1. Unlock the keypad (**Lock**), select **Thread** (or **Thread ↺** for a left-hand thread) with **Mode**, and choose the pitch with the encoder / **Rate ±**.
2. Position the carriage and **hold Jog Left/Right** to set your **end stops**.
3. Move to the starting stop, press **Enable** — the leadscrew waits for the spindle sync point, then tracks the thread to the far stop, where it stops automatically. Retract, return, and **Enable** again for the next pass; sync is preserved so every pass follows the same helix.

### Over-the-air (web) firmware update

**Hold the Half Nut button** to start an OTA update: the device downloads the firmware binary from the
**Update URL** configured on the web page (see [Settings reference](#settings-reference)) and flashes
itself, showing progress on the display. Publish new firmware to that URL to update in the field
without opening the enclosure. *(The wired path — `pio run -e esp32dev_usb -t upload` over the J5
serial header — remains available too.)*

---

## Settings reference

All of the following are exposed on the web configuration page
([`src/WebSettings.cpp`](src/WebSettings.cpp)); the lathe parameters and their defaults are defined in
[`lib/config/latheconfig.h`](lib/config/latheconfig.h).

### WiFi / update settings

| Web label | Field | Meaning |
|---|---|---|
| **SSID** | `ssid` | WiFi network name the device joins (or serves) for connectivity. Up to 31 chars. |
| **Password** | `password` | WiFi password. Up to 62 chars. |
| **Update URL** | `url` | HTTP(S) URL of a firmware `.bin` used for OTA updates. Example default in the UI: `http://hass.longhome.co.uk/els/elstft.bin` — **change this to your own** update endpoint. |

### Lathe / motion settings

| Web label | Field | Default | Units | Meaning |
|---|---|---|---|---|
| **Encoder PPR** | `spindleEncoderPpr` | `1200` | pulses/rev | Pulses per revolution of the spindle encoder. Must match your encoder for correct thread/feed tracking. |
| **Stepper PPR** | `stepperPpr` | `400` | steps/rev | Steps per revolution of the leadscrew stepper motor (i.e. full-steps × microstepping set on the driver). |
| **Invert motor direction** | `invertDirection` | `true` (checked) | boolean | Flips the stepper direction sense so that "right" and "left" match your lathe's geometry. |
| **Gearbox ratio numerator** | `gearboxRatioNumerator` | `2` | ratio | Numerator of any mechanical reduction between the stepper and the leadscrew. |
| **Gearbox ratio denominator** | `gearboxRatioDenominator` | `1` | ratio | Denominator of the stepper-to-leadscrew reduction. Effective ratio = numerator ÷ denominator (default 2:1). |
| **Leadscrew pitch** | `leadscrewPitchMm` | `2.54` | mm/rev | Pitch (lead) of the physical leadscrew — millimetres of carriage travel per leadscrew revolution. |
| **Max jog speed** | `jogSpeed` | `40` | mm/s | Speed used when jogging the carriage. *(The UI labels the unit "m/s"; the value is millimetres per second.)* |
| **Leadscrew acceleration** | `leadscrewAcceleration` | `150` | mm/s² | Acceleration/deceleration ramp used for leadscrew motion. *(UI labels it "m/s²"; value is mm/s².)* |
| **Max leadscrew speed** | `leadscrewMaxSpeed` | `40` | mm/s | Upper speed limit for leadscrew motion. *(UI labels it "m/s"; value is mm/s.)* |

> The firmware derives internal quantities (steps/mm, pulses/sec, acceleration in pulses, initial
> pulse delay) from these values — see [`lib/config/latheconfig.cpp`](lib/config/latheconfig.cpp).
> A fixed jerk limit (`LEADSCREW_JERK`, the max instantaneous start speed) and the built-in
> metric/imperial thread and feed pitch tables live in [`lib/config/config.h`](lib/config/config.h).

---

## Testing / development

The motion/business logic is designed to be unit-testable on your host machine without any hardware,
using the **`native`** PlatformIO environment (the project's default env), which builds against
GoogleTest/GMock:

```sh
pio test -e native
```

`native` is built with `-D PIO_UNIT_TESTING=1`, which selects host test doubles in place of the
ESP32 hardware classes. See the [`test/`](test/) directory for the test suites.

---

## License

This project is licensed under the **GNU General Public License v3.0**. See the [`LICENSE`](LICENSE)
file for the full text.

You are free to use, study, share and modify this software under the terms of the GPLv3. Derivative
works and distributed modified versions must also be made available under the GPLv3.

---

## Attribution

This firmware is **derived from the "TeensyELS" project by NoEngineerHere** (associated with the
*Not an Engineer* YouTube channel), with thanks to the upstream author for the original work this
builds on:

- **Upstream repository:** https://github.com/NoEngineerHere/TeensyELS

The derivation is evidenced by the matching repository name, the near-verbatim original `README.md`
stub, a shared project structure (PlatformIO project, `native` GoogleTest environment, `kicad/`
hardware directory), and this repo's history migrating from Teensy to ESP32 (Teensy support removed in
commit `cbd1b22`).

**Licensing:** the project owner has confirmed that release under the GPLv3 is cleared.

> A separate, well-known project — **Clough42 "electronic-leadscrew"** by James Clough
> (https://github.com/clough42/electronic-leadscrew) — is a *different* ELS design (TI F280049C
> microcontroller, MIT-licensed) and is **not** the upstream of this code; it is noted only to avoid
> confusion with the best-known open ELS project.

---

## Safety disclaimer

**This project controls moving machinery and is provided with no warranty of any kind.**

A lathe with a powered leadscrew can cause **serious injury or death** and can destroy tooling and
workpieces. An electronic leadscrew removes the mechanical safeguards of change gears — a software
bug, miswired encoder, wrong setting, or loss of spindle sync can drive the carriage unexpectedly,
including into the chuck.

- Use at your **own risk**. You are responsible for the safe design, wiring, configuration and
  operation of your machine.
- **Test thoroughly with the spindle stopped and, where possible, the leadscrew disconnected** before
  cutting.
- Always know where your **stop positions** and emergency stop are before engaging.
- This firmware is **experimental** and, as noted above, may contain bugs affecting speed and
  acceleration limits.

As stated in the GPLv3, this software is distributed in the hope that it will be useful, but
**WITHOUT ANY WARRANTY**; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.
