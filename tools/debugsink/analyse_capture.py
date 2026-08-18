#!/usr/bin/env python3
"""Read one ELS motion-trace capture and print what matters for the glitch hunt.

    python3 analyse_capture.py captures/capture-20260818-201500-A1B2C3.csv

Stdlib only. The reported symptom is two things at once - frequent small
(audible) jitter, and a big glitch every 2-10 s where the leadscrew reverses
about 180 degrees rapidly and then carries on - so this prints, in order:

  1. what the capture actually is (samples, span, effective rate);
  2. the FOLLOWING ERROR distribution, and the largest excursions with their
     timestamps, so "how bad and when" is answerable without a spreadsheet;
  3. every DIRECTION REVERSAL, with how far the carriage went before reversing
     back - in pulses AND in degrees of leadscrew rotation, which is the units
     the symptom was reported in;
  4. the INTERVALS between those reversals, which is the number that says
     whether what was captured is the reported 2-10 s glitch or something else.

Pass --steps-per-rev if the machine is not the default (stepperPpr 400 x
gearbox 2 = 800 leadscrew pulses per revolution).
"""

import argparse
import csv
import statistics
import sys

BASE_COLUMNS = [
    "time", "posError", "posErrorRaw", "pulseToTarget", "pos", "expectedPos",
    "speed", "direction", "targetSpeed", "speedDiff", "timeToTarget",
]

# Appended later, for the starvation hypothesis. A capture taken before they
# existed is still perfectly readable - it just cannot answer that question, and
# says so rather than inventing zeroes.
STARVATION_COLUMNS = ["loopGapUs", "spindleDelta"]

COLUMNS = BASE_COLUMNS + STARVATION_COLUMNS

INT_COLUMNS = {"time", "pos", "direction", "loopGapUs", "spindleDelta"}

# Mirrors kStallGapMicros / kSpindleDeltaTrigger in
# lib/global_state/debugcapture.h - the thresholds the device forces a sample
# on. Kept here so the report can say "the device thought this was an event".
STALL_GAP_US = 2000
SPINDLE_DELTA_TRIGGER = 8


def load(path):
    rows = []
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if header is None:
            raise SystemExit("%s: empty file" % path)
        header = [h.strip() for h in header]
        if header == COLUMNS:
            names = COLUMNS
            has_starvation = True
        elif header == BASE_COLUMNS:
            names = BASE_COLUMNS
            has_starvation = False
        else:
            raise SystemExit(
                "%s: unexpected header\n  got      %s\n  expected %s"
                % (path, ",".join(header), ",".join(COLUMNS))
            )
        for lineno, raw in enumerate(reader, start=2):
            if not raw or len(raw) != len(names):
                continue
            row = {}
            try:
                for name, value in zip(names, raw):
                    row[name] = int(value) if name in INT_COLUMNS else float(value)
            except ValueError:
                sys.stderr.write("skipping malformed line %d\n" % lineno)
                continue
            rows.append(row)
    if not rows:
        raise SystemExit("%s: no data rows" % path)
    return rows, has_starvation


def seconds(row):
    return row["time"] / 1e6


def quantile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def find_excursions(rows, threshold):
    """Contiguous runs where |posError| >= threshold, worst first."""
    runs = []
    current = None
    for index, row in enumerate(rows):
        magnitude = abs(row["posError"])
        if magnitude >= threshold:
            if current is None:
                current = {"start": index, "end": index, "peak": row,
                           "peak_index": index, "peak_abs": magnitude}
            else:
                current["end"] = index
                if magnitude > current["peak_abs"]:
                    current["peak"] = row
                    # Kept explicitly: rows.index() would compare dicts by
                    # value and could land on an earlier identical sample.
                    current["peak_index"] = index
                    current["peak_abs"] = magnitude
        elif current is not None:
            runs.append(current)
            current = None
    if current is not None:
        runs.append(current)
    runs.sort(key=lambda run: run["peak_abs"], reverse=True)
    return runs


def find_reversals(rows):
    """Every change of commanded direction, ignoring passes through 0 (UNKNOWN).

    A reversal is recorded when the direction becomes non-zero and differs from
    the last non-zero direction seen - so +1 -> 0 -> -1 is ONE reversal, which
    is what it is mechanically, rather than two.
    """
    events = []
    last_direction = 0
    last_index = None
    for index, row in enumerate(rows):
        direction = row["direction"]
        if direction == 0:
            continue
        if last_direction != 0 and direction != last_direction:
            events.append(
                {
                    "index": index,
                    "row": row,
                    "from": last_direction,
                    "to": direction,
                    "since_index": last_index,
                }
            )
        last_direction = direction
        last_index = index
    return events


def report_starvation(rows, excursions, has_starvation, top):
    """The section this whole capture exists for.

    The hypothesis: SpindleTask is being starved, so it misses hot-loop
    iterations, spindle counts pile up in the encoder, the next
    consumePosition() returns a large delta, m_expectedPosition leaps forward by
    delta x ratio, and the leadscrew rushes to catch up. Every link in that
    chain has a column here, so the CAUSAL ORDER is printed rather than
    inferred: for each large error excursion, the loop gap and spindle delta at
    the peak AND on the samples leading up to it.

    Read it this way:
      gap spike -> delta spike -> error spike  = starvation, and nothing else.
      error spike with flat gap and flat delta = the motion maths, and the
                                                 re-sync gate is the suspect.
    """
    print("")
    print("STARVATION CORRELATION  (the question this capture exists to answer)")
    if not has_starvation:
        print("  This capture predates the loopGapUs / spindleDelta columns, so")
        print("  the starvation hypothesis cannot be tested against it. Take a")
        print("  fresh capture with current firmware.")
        return

    gaps = [row["loopGapUs"] for row in rows]
    deltas = [abs(row["spindleDelta"]) for row in rows]
    median_gap = statistics.median(gaps)
    stalls = [row for row in rows if row["loopGapUs"] >= STALL_GAP_US]
    print("  loop gap:      median %d us   p99 %d us   max %d us"
          % (median_gap, quantile(gaps, 0.99), max(gaps)))
    print("  gaps >= %d us: %d of %d samples   (the device forces a sample on"
          " these)" % (STALL_GAP_US, len(stalls), len(rows)))
    print("  |spindleDelta|: median %d   p99 %d   max %d   (trigger %d)"
          % (statistics.median(deltas), quantile(deltas, 0.99), max(deltas),
             SPINDLE_DELTA_TRIGGER))
    print("  NOTE: both columns are PEAK-HELD between samples - each row reports")
    print("        the worst its window saw, so decimation cannot hide a stall.")

    if not excursions:
        if stalls:
            print("")
            print("  The loop DID stall (%d times) but the following error never"
                  % len(stalls))
            print("  left its band - so the stalls are real but are not (yet)")
            print("  producing the fault.")
        return

    # Is a stall "big" here? Either the device's own trigger, or well clear of
    # this machine's own background - whichever is larger, so a machine with a
    # naturally slow loop is not diagnosed by its own noise floor.
    gap_bar = max(float(STALL_GAP_US), median_gap * 5.0)

    starved = 0
    considered = excursions[:top]
    for number, run in enumerate(considered, start=1):
        peak_index = run["peak_index"]
        first = max(0, peak_index - 4)
        last = min(len(rows) - 1, peak_index + 2)
        peak = run["peak"]
        print("")
        print("  EXCURSION %d  peak posError %.1f at t=%.4f s"
              % (number, peak["posError"], seconds(peak)))
        print("        t(s)     gap(us)    delta     posError        pos   dir")
        for index in range(first, last + 1):
            row = rows[index]
            marker = "  <-- peak" if index == peak_index else ""
            print("   %9.4f %11d %8d %12.1f %10d %5d%s"
                  % (seconds(row), row["loopGapUs"], row["spindleDelta"],
                     row["posError"], row["pos"], row["direction"], marker))

        gap_spike = peak["loopGapUs"] >= gap_bar
        delta_spike = abs(peak["spindleDelta"]) >= SPINDLE_DELTA_TRIGGER
        # Also look just BEFORE the peak: the cause can land one sample earlier
        # than the error it produces.
        lead = rows[first:peak_index]
        lead_gap = max([row["loopGapUs"] for row in lead], default=0)
        lead_delta = max([abs(row["spindleDelta"]) for row in lead], default=0)
        if gap_spike or lead_gap >= gap_bar:
            starved += 1
            worst_gap = max(peak["loopGapUs"], lead_gap)
            worst_delta = max(abs(peak["spindleDelta"]), lead_delta)
            ratio = (worst_gap / median_gap) if median_gap else 0.0
            print("    verdict: loop gap %d us (%.0fx the median) at or just"
                  " before the peak," % (worst_gap, ratio))
            if delta_spike or lead_delta >= SPINDLE_DELTA_TRIGGER:
                print("             with a spindle delta of %d alongside it"
                      % worst_delta)
            else:
                print("             though the spindle delta stayed small (%d)"
                      % worst_delta)
            print("             -> consistent with SpindleTask starvation")
        elif delta_spike or lead_delta >= SPINDLE_DELTA_TRIGGER:
            print("    verdict: spindle delta spiked to %d with NO loop-gap"
                  % max(abs(peak["spindleDelta"]), lead_delta))
            print("             spike - counts piled up without the motion loop")
            print("             stalling. Suspect the encoder/spindle path, not")
            print("             CPU starvation.")
        else:
            print("    verdict: loop gap and spindle delta both flat through the")
            print("             excursion -> NOT starvation. The error was")
            print("             generated by the motion maths; the re-sync gate")
            print("             in Leadscrew::update() is the prime suspect.")

    print("")
    if starved == len(considered):
        print("  VERDICT: all %d excursion(s) examined coincide with a loop-gap"
              % len(considered))
        print("  spike. That is starvation of SpindleTask - look at what else"
              " runs")
        print("  on core 0 (WiFi, flash access, the display) rather than at the"
              " motion maths.")
    elif starved == 0:
        print("  VERDICT: none of the %d excursion(s) examined coincide with a"
              % len(considered))
        print("  loop-gap spike. The hot loop was healthy; the fault is in the")
        print("  motion maths. Start with the re-sync gate and the direction")
        print("  latch in Leadscrew::update().")
    else:
        print("  VERDICT: MIXED - %d of %d excursion(s) coincide with a loop-gap"
              % (starved, len(considered)))
        print("  spike. Two mechanisms, or one that only sometimes stalls. Read")
        print("  the per-excursion blocks above rather than trusting a summary.")


def describe_intervals(label, values, unit="s"):
    if not values:
        print("  (fewer than two %s - no interval to report)" % label)
        return
    print(
        "  n=%d  min %.2f%s  median %.2f%s  mean %.2f%s  max %.2f%s"
        % (
            len(values),
            min(values), unit,
            statistics.median(values), unit,
            statistics.fmean(values) if hasattr(statistics, "fmean")
            else sum(values) / len(values), unit,
            max(values), unit,
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="a .csv written by debugsink.py")
    parser.add_argument("--steps-per-rev", type=float, default=800.0,
                        help="leadscrew pulses per leadscrew revolution "
                             "(stepperPpr x gearbox ratio; default 800)")
    parser.add_argument("--error-threshold", type=float, default=None,
                        help="pulses; excursion cutoff. Default: 10x the median "
                             "|posError|, floored at 5 pulses")
    parser.add_argument("--top", type=int, default=10,
                        help="how many excursions / reversals to list")
    args = parser.parse_args()

    rows, has_starvation = load(args.capture)
    span = seconds(rows[-1]) - seconds(rows[0])
    rate = (len(rows) - 1) / span if span > 0 else 0.0

    print("=" * 72)
    print(args.capture)
    print("=" * 72)
    print("  %d samples spanning %.2f s  (%.0f samples/s average)"
          % (len(rows), span, rate))
    print("  carriage moved from %d to %d pulses (%+d)"
          % (rows[0]["pos"], rows[-1]["pos"], rows[-1]["pos"] - rows[0]["pos"]))

    # --- 2. following error -------------------------------------------------
    magnitudes = [abs(row["posError"]) for row in rows]
    median_error = statistics.median(magnitudes)
    worst = max(rows, key=lambda row: abs(row["posError"]))
    print("")
    print("FOLLOWING ERROR (posError, pulses - the planner's own error term)")
    print("  median %.2f   p95 %.2f   p99 %.2f   max %.1f at t=%.4f s"
          % (median_error, quantile(magnitudes, 0.95), quantile(magnitudes, 0.99),
             abs(worst["posError"]), seconds(worst)))
    raw_worst = max(rows, key=lambda row: abs(row["posErrorRaw"]))
    print("  posErrorRaw (before the speed-matching term): max %.1f at t=%.4f s"
          % (abs(raw_worst["posErrorRaw"]), seconds(raw_worst)))

    threshold = args.error_threshold
    if threshold is None:
        threshold = max(5.0, median_error * 10.0)
    print("")
    print("LARGEST ERROR EXCURSIONS  (runs of |posError| >= %.1f pulses)" % threshold)
    excursions = find_excursions(rows, threshold)
    if not excursions:
        print("  none - the following error never left the band")
    else:
        print("     start(s)   dur(ms)     peak    at pos      dir    speed")
        for run in excursions[: args.top]:
            start = seconds(rows[run["start"]])
            duration = (seconds(rows[run["end"]]) - start) * 1000.0
            peak = run["peak"]
            print("   %10.4f %9.1f %8.1f %9d %8d %8.0f"
                  % (start, duration, peak["posError"], peak["pos"],
                     peak["direction"], peak["speed"]))
        if len(excursions) > args.top:
            print("   ... and %d more" % (len(excursions) - args.top))

    # --- 2b. the starvation correlation ------------------------------------
    report_starvation(rows, excursions, has_starvation, args.top)

    # --- 3. direction reversals --------------------------------------------
    print("")
    print("DIRECTION REVERSALS  (the reported symptom: a rapid ~180 deg reverse)")
    reversals = find_reversals(rows)
    if not reversals:
        print("  none - the commanded direction never flipped")
    else:
        print("   %d reversal(s). TRAVEL is the extent the carriage covered on"
              % len(reversals))
        print("   the leg that starts at each reversal - i.e. how far it went"
              " that way")
        print("   before reversing again (or before the capture ended).")
        print("")
        print("        at(s)    dir       travel      degrees   posError    speed")
        for number, event in enumerate(reversals):
            row = event["row"]
            end = (reversals[number + 1]["index"] if number + 1 < len(reversals)
                   else len(rows) - 1)
            leg = [item["pos"] for item in rows[event["index"]:end + 1]]
            # Extent, not endpoint-to-endpoint: a leg that goes out and comes
            # most of the way back would otherwise read as barely moving, and
            # it is the OUTWARD distance the symptom is described in.
            travel = max(leg) - min(leg) if leg else 0
            degrees = 360.0 * travel / args.steps_per_rev
            print("   %10.4f  %+d->%+d %9d %12.1f %10.1f %8.0f"
                  % (seconds(row), event["from"], event["to"], travel, degrees,
                     row["posError"], row["speed"]))
            if number + 1 >= args.top:
                remaining = len(reversals) - args.top
                if remaining > 0:
                    print("   ... and %d more" % remaining)
                break

        # --- 4. the interval between them ----------------------------------
        times = [seconds(event["row"]) for event in reversals]
        gaps = [second - first for first, second in zip(times, times[1:])]
        print("")
        print("INTERVAL BETWEEN REVERSALS  (every reversal, including both halves")
        print("of an out-and-back - so a glitch contributes one short gap and one")
        print("long one. The out-and-back interval below is the one to compare")
        print("against the reported 2-10 s.)")
        describe_intervals("reversals", gaps)

        # Reversal pairs that came straight back - the "reverses then carries
        # on" shape, as opposed to an ordinary change of travel direction.
        pairs = []
        for first, second in zip(reversals, reversals[1:]):
            back_ms = (seconds(second["row"]) - seconds(first["row"])) * 1000.0
            if second["to"] == first["from"] and back_ms <= 1000.0:
                leg = [item["pos"]
                       for item in rows[first["index"]:second["index"] + 1]]
                travel = max(leg) - min(leg) if leg else 0
                pairs.append((seconds(first["row"]), back_ms, travel,
                              360.0 * travel / args.steps_per_rev))
        print("")
        print("OUT-AND-BACK REVERSALS  (reversed and returned within 1 s - the"
              " glitch shape)")
        if not pairs:
            print("  none")
        else:
            print("        at(s)   back after(ms)    pulses     degrees")
            for at, back_ms, travel, degrees in pairs[: args.top]:
                print("   %10.4f %15.1f %9d %11.1f"
                      % (at, back_ms, travel, degrees))
            if len(pairs) > args.top:
                print("   ... and %d more" % (len(pairs) - args.top))
            gaps = [b - a for a, b in zip([p[0] for p in pairs],
                                          [p[0] for p in pairs][1:])]
            print("  interval between out-and-back events:")
            describe_intervals("events", gaps)

    # --- extra: position discontinuities ------------------------------------
    # A capture is decimated, so `pos` is expected to move between samples - but
    # a jump far larger than the speed at that moment can explain is the
    # signature of a lost/duplicated batch of steps rather than a planned move.
    print("")
    print("POSITION JUMPS  (|pos| change per sample, largest first)")
    jumps = []
    for previous, current in zip(rows, rows[1:]):
        dt = (current["time"] - previous["time"]) / 1e6
        if dt <= 0:
            continue
        delta = current["pos"] - previous["pos"]
        implied = abs(delta) / dt  # pulses/s the position implies
        jumps.append((abs(delta), delta, seconds(current), dt * 1000.0, implied,
                      current["speed"]))
    jumps.sort(reverse=True)
    print("        at(s)      dt(ms)     dpos   implied pps   planner pps")
    for _, delta, at, dt_ms, implied, planner in jumps[: args.top]:
        print("   %10.4f %11.2f %8d %13.0f %13.0f"
              % (at, dt_ms, delta, implied, planner))
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
