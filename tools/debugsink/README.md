# ELS motion-trace capture — sink and analysis

The lathe records a trace of the leadscrew planner from inside its hot loop and
POSTs it here when the carriage stops. This directory is the receiving end plus
the script that reads the result.

## 1. Run the sink on hass.longhome.co.uk

```sh
python3 debugsink.py                                # 0.0.0.0:8088, ./captures
python3 debugsink.py --port 8088 --dir /srv/els-captures
```

Python 3.7+, standard library only — nothing to install. It prints one line per
capture:

```
20260818-201500  capture-20260818-201500-A1B2C3D4.csv  2400 rows  59.8 s  17 direction reversals  fw v0.2.1  (198431 bytes)
```

Leave it running (`tmux`, `screen`, or a systemd unit — a sample one is at the
bottom of this file).

## 2. Configure the device

Boot the ELS into setup mode (MENU → Wi-Fi setup, or hold OK at power-on), join
the **ELS_Wifi** access point, open the setup page, and set:

| Field | Value |
| --- | --- |
| **Debug capture URL** | `http://hass.longhome.co.uk:8088/capture` |

That exact URL is also the field's default, so on a device that has just been
reconfigured from scratch it is already filled in — check it and press Save.

**Prefer `http://` over `https://`.** The device holds the ~105 KB trace in RAM
while it uploads; a TLS session plus the CA bundle costs another ~45 KB on top,
on a board with ~200 KB of free DRAM. Plain HTTP to a machine on your own
network removes that risk entirely. (`https://` does work — the same CA bundle
the OTA updater uses — if the sink is only reachable over the internet.)

Leave the field blank to disable uploading. The capture still records and the
Diagnostics screen still shows it filling; it simply has nowhere to go.

> **This firmware bumps `CHECKVALUE`.** The stored-settings layout changed to
> make room for the URL above, so the first boot after flashing wipes the saved
> Wi-Fi credentials *and every commissioned lathe measurement* and comes up as
> the `ELS_Wifi` access point. Write down the geometry values before flashing.

## 3. Take a capture

1. **Stop the carriage.** The capture tile is refused while anything is moving —
   it allocates ~105 KB, and a trace armed after the cut starts would not
   contain the cut.
2. `MENU` → **Debug capture** → `OK`. The menu closes onto the Diagnostics
   screen, whose title row now reads `REC 0/2400`.
3. Make the cut. The buffer spans about 60 seconds.
   *The Diagnostics screen closes itself the moment the carriage moves* — that
   is the panel's motion lockout ("all of the operator's attention should be on
   the tool, not the screen"), and it applies here like everywhere else. The
   capture carries on regardless; reopen `MENU` → `Diagnostics` once you have
   stopped to see where it got to.
4. When it fills, the capture stops itself. The title reads `FULL: STOP TO SEND`.
5. **Stop the carriage.** As soon as the axis is genuinely at rest (motion
   disabled *and* the deceleration ramp finished) the device connects to Wi-Fi
   on its own and sends: `SENDING TRACE`, then `SENT 2400`.
6. If it says `SEND FAILED` the trace is still in RAM — stop the carriage again
   any time after 30 seconds and it retries. Pressing `OK` on the tile at that
   point throws the trace away instead.

The upload **suspends step generation** for its duration (the Wi-Fi driver and
the motion loop share a CPU core), and the machine is left disengaged
afterwards, so press `ENABLE` again to re-engage. This is why it waits for the
carriage to stop rather than sending as soon as the buffer is full.

## 4. Read the capture

```sh
python3 analyse_capture.py captures/capture-20260818-201500-A1B2C3D4.csv
```

It prints, in this order:

* what the trace is — sample count, span in seconds, effective sample rate, and
  how far the carriage moved overall;
* the **following-error distribution** (median / p95 / p99 / max with its
  timestamp), for both `posError` and the raw `posErrorRaw` before the
  speed-matching term is added;
* the **largest error excursions** — contiguous runs above a threshold, worst
  first, each with its start time, duration, peak error, position, direction and
  planner speed;
* every **direction reversal**, with how far the carriage travelled before the
  next one, in pulses *and in degrees of leadscrew rotation* (the units the
  symptom was reported in — pass `--steps-per-rev` if the machine is not the
  default 400 × 2 = 800);
* the **interval between reversals** (n, min, median, mean, max) — the number
  that says whether what was captured is the reported 2–10 s glitch;
* **out-and-back reversals**: a reversal that returned within a second, which is
  the "reverses ~180° then carries on" shape specifically, as opposed to an
  ordinary change of travel direction;
* the largest **position jumps** per sample, with the pulse rate they imply next
  to the planner's own commanded speed — a jump the speed cannot explain is the
  signature of lost or duplicated steps rather than a planned move.

Useful flags: `--top N` (how many rows per table), `--error-threshold PULSES`
(the excursion cutoff; defaults to 10× the median error, floored at 5 pulses).

## 5. Wire format

`text/csv`, one header line then one row per sample:

```
time,posError,posErrorRaw,pulseToTarget,pos,expectedPos,speed,direction,targetSpeed,speedDiff,timeToTarget
```

* `time` — **microseconds since the capture was armed**, not raw `micros()`.
* `pos` — carriage position in leadscrew pulses; `expectedPos` is where the
  spindle says it should be.
* `posError` = `posErrorRaw` + `pulseToTarget`, all in pulses.
* `speed` / `targetSpeed` — planner and target, in pulses per second.
* `direction` — commanded direction: `-1` left, `+1` right, `0` unknown.

Sent chunked (`Transfer-Encoding: chunked`) with these headers, which the sink
uses for the filename and the summary line: `X-ELS-Device` (MAC),
`X-ELS-Version` (firmware), `X-ELS-Samples`.

CSV rather than a binary blob on purpose: the sink writes the body straight to a
file with no decoder, there is no endianness or float-layout contract to get
wrong, and the capture opens in a spreadsheet when the analysis says something
surprising. ~215 KB per capture over Wi-Fi costs nothing.

## 6. PHP drop-in (optional)

If there is already a web server on the box, `capture.php` does the same job —
drop it somewhere served and point the device at it instead. Note that PHP
receives an already-de-chunked body, so it is a much shorter file; the Python
version is the one to prefer, since it needs no web server at all.

## 7. systemd unit (optional)

```ini
[Unit]
Description=ELS motion-trace capture sink
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/els/debugsink.py --port 8088 --dir /srv/els-captures
Restart=always
User=els

[Install]
WantedBy=multi-user.target
```
