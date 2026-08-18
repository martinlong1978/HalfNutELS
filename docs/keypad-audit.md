# Keypad audit — why it drops presses

Written against `src/keyarray.cpp` / `src/keyarray.h` at `656e0e9`, in answer to
"there has been some degree of unreliability in the keypad… make sure it's
watertight… is the debouncing logic causing problems?"

Short answer: yes, the debounce is causing it, and there is a smoking gun. But
the debounce is only the trigger — the underlying design couples debouncing to
interrupt *re-arming*, and that is what turns a swallowed edge into a dead
keypad. Tuning the 10 ms constant would not fix it.

## 1. The smoking gun: a swallowed release strands the interrupt arming

The matrix is scanned by `getCodeFromArray()`, which ends with:

```cpp
setupKeys(code == 0);      // true  -> attach RISING  (wait for a press)
                           // false -> attach FALLING (wait for a release)
```

So the pad is only ever armed for *one* edge direction at a time, and the
direction is chosen as a side effect of the last scan. Press and release share
one debounce timestamp and one 10 ms window:

```cpp
void KeyArray::handle()        { if (time < keycodeMillis + 10) return; ... }
void KeyArray::handleRelease() { if (time < keycodeMillis + 10) return; ... }
```

Now take a tap where the key is released less than 10 ms after it was pressed —
a light quick press, or a contact that opens briefly on the way in:

1. **Press.** RISING fires → `handle()` → scan returns a non-zero code →
   `setupKeys(false)` → the pad is now armed for **FALLING**. `keycodeMillis` is
   set to now. The hold timer starts.
2. **Release, <10 ms later.** FALLING fires → `handleRelease()` → the debounce
   test is true → **it returns immediately**. Three things do not happen:
   `setupKeys(true)` is never called, so the pad stays armed for FALLING; no
   `BS_CLICKED` and no `BS_RELEASED` are emitted; and `buttonState` is left at
   `BS_PRESSED`.
3. The key is now physically up, so the line is already low. **No further
   FALLING edge can ever occur.**
4. The next press produces a RISING edge — which nothing is listening for.

**The keypad is dead.** It recovers only when the 1 s hold timer fires, because
`handleTimer()` happens to call `getCodeFromArray()` again, which re-arms via
`setupKeys(code == 0)`. So the observable symptom is *"sometimes a press does
nothing, and then it's fine again"* — with a dead window of up to a second.

This is consistent with the holes already noted in `buttonpad.cpp` ("the
debounce and one-second-rescan holes"), but the consequence is worse than a
dropped event: it is a dropped event **plus** a period where the pad cannot
report anything at all.

### 1a. The same fault can leave a jog running

`UiState` starts a dead-man jog on the arrow **Press** and stops it on the
**Release**. In the sequence above the Release is never emitted, and the 1 s
timer resets `buttonState` to `BS_NONE` silently — `handleTimer()` emits nothing
on that path. So the `JogStop` never arrives.

The window is narrow (the release must land inside 10 ms of the press) and a
deliberate human tap is normally 50–150 ms, so this is unlikely to be what has
been seen day to day. It is still a motion command that can be lost, which is
why the fix below removes the possibility rather than narrowing it.

## 2. ISR-unsafe calls, on every single key event

`getCodeFromArray()` calls `pinMode()` six times and `setupKeys()` calls
`attachInterrupt()` three times. Both run **inside** the GPIO ISR (via
`handle()` / `handleRelease()`) and inside the timer ISR (via `handleTimer()`).

`attachInterrupt()` reaches `gpio_isr_handler_add()`, which takes a spinlock and
may allocate; `pinMode()` takes the GPIO lock. Calling either from interrupt
context is unsupported, and the failure mode is a deadlock or a corrupted
handler table rather than anything that announces itself.

Worse for this machine specifically: `buttonInterrupt()`, `buttonInterruptRelease()`,
`handle()`, `handleRelease()`, `handleTimer()`, `getCodeFromArray()` and
`setupKeys()` are **all in flash**, and none is `IRAM_ATTR` (only the trivial
`timerInterrupt` wrapper is). Whenever the flash cache is disabled — every
settings save, every OTA, every capture upload — a keypress in that window
executes unmapped code. We now know all three of those happen regularly.

## 3. A full ring buffer is indistinguishable from an empty one

```cpp
void KeyArray::emitButton() {
  ringBuffer[writeindex] = ...;
  writeindex = (writeindex + 1) % 10;   // no fullness check
}
ButtonInfo KeyArray::consumeButton() {
  if (readindex == writeindex) return { 0, BS_NONE };   // "empty"
  ...
}
```

Ten queued events wrap `writeindex` onto `readindex`, and the reader then sees
the buffer as *empty* — so a burst does not drop the oldest event, it drops
**all ten**. `ButtonPad::handle()` drains at most 10 per pass and the pass is
100 ms apart, so a chattering contact can reach this.

The ring entries are also plain (non-`volatile`) structs written from an ISR and
read from a task, with no ownership protocol beyond the two indices — a reader
can in principle observe a half-written entry.

## 4. Why tuning the constant does not fix it

The 10 ms figure is not really a debounce. A debounce answers *"has the input
settled?"*; this answers *"has it been long enough since the last edge I acted
on?"* — a lockout, keyed on a timestamp shared between two different transitions,
and used to gate the code that re-arms the hardware.

* **Raise it** and more releases fall inside the window — more dead keypad.
* **Lower it** and contact bounce gets through as real events, producing double
  presses and spurious clicks.

There is no value that is safe at both ends, because the two failures are driven
by the same number in opposite directions. That is the definition of a design
that needs replacing rather than tuning.

## 5. The fix: scan on a timer, integrate, and stop using edge interrupts

Replace edge-triggered interrupts with a **polled scan** on a small dedicated
task, and debounce by *integration* — a code must read the same for several
consecutive samples before it is believed. This is the standard approach for a
mechanical matrix, and it removes every fault above by construction:

* **No `attachInterrupt()` at all**, so nothing re-arms anything and §1 cannot
  happen. There is no arming state to strand.
* **No ISR-unsafe calls**, because there is no ISR — the scan runs in task
  context where `pinMode()` and `digitalRead()` are legal, and flash-cache
  windows stop mattering (§2).
* **Bounce is filtered rather than raced.** A contact that chatters for 5 ms
  simply never reaches the stable-sample threshold; it does not consume a
  timestamp that some later transition depends on.
* **Both edges are always observable**, because every sample sees the whole
  matrix. A tap shorter than the debounce is not "swallowed leaving the pad
  dead" — it either registers as a complete Press/Click/Release or is rejected
  as noise, and the next sample is unaffected either way.

The event vocabulary is unchanged — `BS_PRESSED` / `BS_CLICKED` / `BS_HELD` /
`BS_RELEASED`, with the same "no Click after a Hold" rule `UiState` is built on
(`lib/ui/uistate.cpp` header comment) — so `ButtonPad` and `UiState` need no
changes and their 437 tests keep their meaning.

The decision logic goes in `lib/keyscan/`, pure C++ taking `(rawCode, nowMs)`
and returning events, so it is host-testable the way `UiState` and `Dro` are.
`KeyArray` keeps only the hardware: drive a column, read the rows, hand the code
to the scanner.

### Scan rate and thresholds

* **Scan period 2 ms.** Fast enough that press latency is imperceptible, slow
  enough to be free (a matrix scan is a handful of GPIO ops).
* **Debounce 4 consecutive equal samples (~8 ms).** Comfortably longer than the
  1–3 ms bounce typical of these switches, and still well under the ~50 ms floor
  of a deliberate human tap.
* **Hold at 1000 ms**, unchanged — `UiState`'s clear-both-stops gesture and the
  OK-hold DRO zero are both specified against it.

## 6. If it still misbehaves after this — how to debug it

The instrument already exists. Add a serial counter per event type
(`Press/Click/Hold/Release`, plus rejected-as-bounce and ring-buffer-full), print
it once a second from the DisplayTask, and press a key a known number of times:
the counts either match the presses or they name which stage lost them. That is
the same "count the thing and read the number" approach that found the RMT
fault, after three theories from reading diffs had failed.

If the counts are right but the UI still misses gestures, the loss is downstream
in `ButtonPad`/`UiState`, and those are host-testable without the hardware.
