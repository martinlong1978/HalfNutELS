# ESP32 ELS — Electronic Leadscrew

An open-source **Electronic Leadscrew (ELS)** controller for a metalworking lathe, running on
an **ESP32** (LilyGO / TTGO T-Display).

An electronic leadscrew replaces a lathe's mechanical change gears / gearbox. Instead of a train of
gears mechanically coupling the spindle to the leadscrew, an **encoder** on the spindle is read by
the microcontroller, which drives a **stepper motor** on the leadscrew in software-defined ratio to
the spindle. This lets you select any thread pitch or feed rate electronically — for single-point
threading and powered feeding — without swapping gears.

> **Note on the name:** the repository is historically called *TeensyELS* because it began life on a
> Teensy microcontroller. Teensy support has since been **removed** — the active firmware targets the
> **ESP32 (LilyGO T-Display)**. See [Attribution](#attribution).

---

## Contents
- [Features](#features)
- [Hardware / Components](#hardware--components)
- [Getting boards made (PCB)](#getting-boards-made-pcb)
- [Building & flashing the firmware](#building--flashing-the-firmware)
- [Connecting & configuring](#connecting--configuring)
- [Settings reference](#settings-reference)
- [Testing / development](#testing--development)
- [License](#license)
- [Attribution](#attribution)
- [Safety disclaimer](#safety-disclaimer)

---

## Features

- **Threading** — synchronise the leadscrew to the spindle at a selectable thread pitch for
  single-point threading. Metric (mm/rev) and imperial (TPI) thread pitch tables are built in.
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
- **On-device display** — a TFT display (LilyGO T-Display, ST7789 240x135) shows current mode, pitch
  and status via LVGL.

---

## Hardware / Components

The controller is built around a custom PCB. KiCad design files, gerbers and fabrication outputs are
in the [`kicad/`](kicad/) directory. The board is provided in **two variants**:

- **2-layer** — [`kicad/2Layer/`](kicad/2Layer/)
- **4-layer** — [`kicad/4Layer/`](kicad/4Layer/) (includes ready-to-order production / assembly files)

### Bill of materials

The parts list below is taken directly from the 4-layer production BOM
([`kicad/4Layer/production/bom.csv`](kicad/4Layer/production/bom.csv)):

| Designator(s) | Qty | Part / Value | Footprint |
|---|---|---|---|
| MCU1 | 1 | **LilyGO TTGO T-Display** (ESP32 + 1.14" ST7789 TFT) — main controller | TTGO-T-Display |
| U1 | 1 | Teensy 4.1 footprint (legacy — see note below) | Teensy4.1 |
| Q1–Q8 | 8 | BSS138 N-channel MOSFET (logic-level shifting) | SOT-23 |
| R1–R8, R11, R12, R18 | 11 | 10 kΩ resistor | 0805 |
| R9, R10 | 2 | 330 Ω resistor | 0805 |
| R13, R14 | 2 | 0 Ω resistor (jumper) | 0805 |
| R15, R16, R17 | 3 | 1 kΩ resistor | 0805 |
| SW1–SW10 | 10 | Omron B3F tactile push button (keypad) | SW_TH_Tactile_Omron_B3F-10xx |
| J1 | 1 | Shielded RJ45 connector (spindle encoder input) | RJ45_RCH_RC01937 |
| J2 | 1 | 4-pin JST-XH connector | JST_XH_B4B-XH-A 1x04 2.50 mm |
| J3 | 1 | 4-pin 2.54 mm pin header | PinHeader 1x04 2.54 mm |
| J4 | 1 | 2-pin 5.08 mm screw terminal (power / stepper) | CUI TB007-508-02 |
| E1 | 1 | Sparkfun-footprint part | Sparkfun |

Not populated on the board but **required to build a working ELS**:

- A **spindle encoder** — a quadrature rotary encoder mounted to the lathe spindle (wired to the RJ45
  input J1). The default configuration assumes **1200 PPR** (see [Settings reference](#settings-reference)).
- A **stepper motor + driver** for the leadscrew (step/dir interface). The board outputs step, dir and
  enable signals; the default configuration assumes a **400 step/rev** stepper.
- A suitable **power supply** for the stepper driver.

> **Note on U1 (Teensy 4.1 footprint):** the 4-layer BOM still carries a Teensy 4.1 footprint from the
> board's Teensy origins. The **active firmware in this repository targets the ESP32 (MCU1, the TTGO
> T-Display)** and Teensy support has been removed from the code. Confirm which controller footprint
> your board revision actually uses before ordering/populating — **please verify against the KiCad
> schematic** ([`kicad/4Layer/TeensyELS.kicad_sch`](kicad/4Layer/TeensyELS.kicad_sch)).

### Pin assignments (ESP32)

Defined in [`lib/config/board.h`](lib/config/board.h):

| Function | GPIO |
|---|---|
| Spindle encoder A / B | 35 / 34 |
| Leadscrew step | 25 |
| Leadscrew direction | 26 |
| Stepper enable | 17 |
| Button-array columns (H1/H2/H3) | 32 / 33 / 2 |
| Button-array rows (V1/V2/V3) | 13 / 14 / 15 |
| TFT display (MOSI/SCLK/CS/DC/RST/BL) | 19 / 18 / 5 / 16 / 23 / 4 |

---

## Getting boards made (PCB)

Fabrication outputs for the 4-layer board are pre-generated under
[`kicad/4Layer/`](kicad/4Layer/). You can order bare boards, or fully assembled boards (PCBA), from a
prototype house.

### Option A — JLCPCB (bare board, or with assembly)

1. Go to [jlcpcb.com](https://jlcpcb.com) and click **Add gerber file**.
2. Upload [`kicad/4Layer/gerbers.zip`](kicad/4Layer/gerbers.zip) (do **not** unzip it — upload the zip
   directly).
3. Confirm the auto-detected settings: **Layers = 4**, and the board dimensions shown in the preview.
   Choose thickness, colour, surface finish, etc. as desired.
4. *(Optional — assembly / PCBA)* Enable **PCB Assembly**, then when prompted upload the assembly
   files from [`kicad/4Layer/production/`](kicad/4Layer/production/):
   - **BOM:** [`kicad/4Layer/production/bom.csv`](kicad/4Layer/production/bom.csv)
   - **Pick-and-place / positions:** [`kicad/4Layer/production/positions.csv`](kicad/4Layer/production/positions.csv)
     (per-side PnP files are also available under [`kicad/4Layer/pnp/`](kicad/4Layer/pnp/)).
   The [`kicad/4Layer/production/TeensyELS.zip`](kicad/4Layer/production/TeensyELS.zip) bundle contains
   the production set for convenience.
5. Review the placement in JLCPCB's assembly preview, resolve any part substitutions, and place the
   order. (The TTGO T-Display module and connectors are typically hand-soldered rather than assembled.)

### Option B — PCBWay

1. Go to [pcbway.com](https://www.pcbway.com) and choose **Quick-order PCB → Add Gerber File**.
2. Upload [`kicad/4Layer/gerbers.zip`](kicad/4Layer/gerbers.zip).
3. Confirm layer count (**4**) and dimensions, pick your board options, and add to cart.
4. *(Optional — assembly)* Choose **Turnkey Assembly** and provide the BOM
   ([`kicad/4Layer/production/bom.csv`](kicad/4Layer/production/bom.csv)) and the pick-and-place file
   ([`kicad/4Layer/production/positions.csv`](kicad/4Layer/production/positions.csv) or the files in
   [`kicad/4Layer/pnp/`](kicad/4Layer/pnp/)).

> The 2-layer variant can be ordered the same way using
> [`kicad/2Layer/gerbers.zip`](kicad/2Layer/gerbers.zip) (select **2 layers**). The 2-layer directory
> does not ship the same ready-made assembly/PnP bundle as the 4-layer one.

> **LCSC part numbers:** the BOM's `LCSC Part #` column is currently blank, so for JLCPCB/PCBWay
> assembly you will need to select/confirm matching parts during checkout. **Please verify** each part
> against your board revision.

---

## Building & flashing the firmware

The firmware is a [PlatformIO](https://platformio.org/) project (see
[`platformio.ini`](platformio.ini)).

### 1. Install PlatformIO

Install the [PlatformIO IDE extension for VS Code](https://platformio.org/install/ide?install=vscode),
or the [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html).
PlatformIO will automatically download the ESP32 toolchain and the libraries listed in
`platformio.ini` on first build.

### 2. Build & upload

There are two ESP32 build environments:

- **`esp32dev_usb`** — build and flash over USB. This is the environment to use for normal
  development. Edit the `upload_port` in `platformio.ini` (default `COM14`) to match your board's
  serial port.

  ```sh
  pio run -e esp32dev_usb -t upload
  ```

- **`esp32dev_publish`** — a release build that uses a custom upload command (`publish.cmd`) for
  producing/publishing an OTA image rather than a direct USB flash.

  ```sh
  pio run -e esp32dev_publish
  ```

Both ESP32 environments target `board = lilygo-t-display` and use a custom 4 MB partition table
(`my_4MB.csv`).

> The `teensy41` / `teensy41_debug` environments remain in `platformio.ini` for historical reasons but
> are **not the supported target** — the code paths for Teensy have been removed.

---

## Connecting & configuring

Configuration is done over WiFi through a built-in web page — there is no need to recompile to
retune the firmware for your lathe.

1. **Enter configuration mode.** The device starts a WiFi **Access Point** on first boot (when no
   valid settings are stored yet) or when you hold the designated config pad while powering on. See
   [`src/main.cpp`](src/main.cpp).
   - AP SSID: **`ELS_Wifi`**
   - AP password: **`123456789`**
2. **Connect** your phone or laptop to that access point.
3. **Open the device in a browser.** The device's IP address is **shown on the on-device display**
   (the AP address is typically `192.168.4.1`). Browse to that address to reach the configuration
   page titled *"ESP32 ELS - Electronic Leadscrew"*.

   > *A captive-portal convenience (auto-opening the config page on connect) may be present depending
   > on firmware version. Regardless, the reliable method is: connect to the `ELS_Wifi` AP and browse
   > to the IP shown on the device's display.*

4. **Fill in the settings** (see [Settings reference](#settings-reference)) and press **Submit**.
   Settings are written to the ESP32's flash. Press **Reset** on the page (or power-cycle) to reboot
   into normal operation with the new settings.

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

> ⚠️ **PLEASE VERIFY — attribution and licensing need confirmation by the project owner before
> publishing.**

This firmware appears to be **derived from / forked from the "TeensyELS" project by
NoEngineerHere** (associated with the *Not an Engineer* YouTube channel):

- **Upstream repository:** https://github.com/NoEngineerHere/TeensyELS

**Evidence supporting this attribution:**

- The repository name (`TeensyELS`) is an exact match.
- The original `README.md` stub in this repo is **near-verbatim** the upstream's README ("This is an
  implementation of an electronic leadscrew using a Teensy microcontroller", "We use platformio as the
  infrastructure to build this project", "run the `pio test --environment native` command").
- Matching project structure: PlatformIO project, `native` test environment (GoogleTest), and a
  `kicad/` hardware directory in both repos.
- This repo's history shows Teensy support being **removed** (commit `cbd1b22`) and the target migrated
  to ESP32 — consistent with a fork that started from a Teensy-based upstream.

**⚠️ Licensing caveat — potential conflict:**

At the time of writing, the upstream repository **`NoEngineerHere/TeensyELS` does not contain a LICENSE
file** (GitHub reports no detectable license). Under default copyright law, "no license" means **all
rights reserved** — it does *not* grant permission to redistribute or relicense the code, and in
particular it is **not automatically compatible with releasing a derivative under GPLv3**.

Before open-sourcing this project under GPLv3, the owner should:

1. **Confirm** whether this codebase is in fact a derivative of `NoEngineerHere/TeensyELS` (or of some
   other upstream).
2. **Contact the upstream author** to obtain explicit permission / an appropriate license, or confirm
   the license under which the code was originally received.
3. Only then finalise the GPLv3 relicensing, and **credit the upstream author** appropriately here.

> A separate, well-known project — **Clough42 "electronic-leadscrew"** by James Clough
> (https://github.com/clough42/electronic-leadscrew) — is a *different* ELS design (TI F280049C
> microcontroller, **MIT-licensed**) and does **not** appear to be the direct upstream of this code,
> despite being the best-known open ELS project. It is noted here only to rule it out; **please verify**.

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
