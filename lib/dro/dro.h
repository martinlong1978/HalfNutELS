// DRO datum resolution - docs/ux-redesign.md section 8, "The DRO datum".
//
// Pure arithmetic over pulse positions. NO Arduino/ESP includes: this library
// must build for the native host so test/test_dro can exercise it.
//
// A position is meaningless without a zero, and "wherever the machine happened
// to boot" is the worst available choice, so zero is referenced to an endstop.
// Resolution order, first match wins:
//
//   1. Manual zero set        -> that position
//   2. Both stops set         -> the one named by `preference`
//   3. Exactly one stop set   -> that one; the preference cannot be honoured
//   4. Neither stop set       -> the power-on origin (0), flagged REL on screen
//
// Sign convention: position increases to the RIGHT, matching
// LeadscrewDirection::RIGHT = 1. A left datum reads 0 at the left and counts up
// rightward; a right datum reads 0 at the right and counts negative leftward.
#pragma once

enum class DroDatumPreference { Left, Right };
enum class DroDatumSource { ManualZero, LeftStop, RightStop, Origin };

// Snapshot of everything the datum rules depend on. The caller is responsible
// for deciding whether a stop/zero is set (it owns the unset sentinels); this
// module takes explicit `...Set` booleans and never inspects the pulse fields
// of an unset entry.
struct DroInput {
  bool leftStopSet;
  int leftStopPulses;
  bool rightStopSet;
  int rightStopPulses;
  bool manualZeroSet;
  int manualZeroPulses;
  DroDatumPreference preference;
};

class Dro {
 public:
  // Which rule wins for this input.
  static DroDatumSource resolveSource(const DroInput& in);

  // Pulse position of the resolved datum; 0 when the source is Origin.
  static int datumPulses(const DroInput& in);

  // Carriage position relative to the datum: currentPulses - datumPulses().
  static int relativePulses(const DroInput& in, int currentPulses);
};
