#include <stdint.h>
#include <stddef.h>

#include "dro.h"

#ifndef ELS_LATHECONFIG_H
#define ELS_LATHECONFIG_H

// Bumped for the ux-redesign theme/DRO-datum fields below (docs/ux-redesign.md
// section 6 "Saving settings from the device"). LatheConfig is memcpy'd raw
// to/from a fixed-size flash region (src/WebSettings.cpp), so any stored blob
// written by older firmware is now shorter than sizeof(LatheConfig) - the new
// `theme`/`droDatum` bytes (and anything after them) would read back as
// whatever flash garbage happened to follow. Bumping CHECKVALUE makes
// `check != CHECKVALUE` so the device discards the stored blob and re-adopts
// struct defaults instead of trusting garbage. Per CLAUDE.md this also wipes
// saved Wi-Fi credentials (WebSettings and LatheConfig share one erased-as-a-
// unit 4 KB sector) - expected, not a bug.
#define CHECKVALUE 0xDFEB0941

// Theme selection stored in LatheConfig::theme. Plain uint8_t rather than
// `enum class` (which has no guaranteed underlying type/size) so the raw
// struct blit to flash has one well-defined byte, portable across compilers
// and optimisation levels.
#define THEME_DARK  0
#define THEME_LIGHT 1

// DRO datum preference stored in LatheConfig::droDatum, mirroring
// DroDatumPreference (lib/dro/dro.h) as a flash-safe raw byte - same
// plain-uint8_t reasoning as `theme` above. 0 = Left (default: the
// conventional Z-zero reference when threading toward the chuck), 1 = Right.
// See latheDroDatumPreference()/fromDroDatumPreference() in latheconfig.cpp
// for the (safe - out-of-range falls back, never cast blindly) mapping onto
// the enum.
#define DRO_DATUM_LEFT  0
#define DRO_DATUM_RIGHT 1

//#define ELS_OFFLINE
typedef struct LatheConfig {
    int32_t check;
    int spindleEncoderPpr = 1200;
    int stepperPpr = 400;
    bool invertDirection = true; // true => right = 1.
    int gearboxRatioNumerator = 2;
    int gearboxRatioDenominator = 1;
    float leadscrewPitchMm = 2.54; //mm per turn
    int jogSpeed = 40; //mm/s
    int leadscrewAcceleration = 150; //mm/s2
    int leadscrewMaxSpeed = 40; // mm/s

    // --- ux-redesign additions (docs/ux-redesign.md section 6 "Saving
    // settings from the device", section 8 "Theme"). Appended at the end of
    // the struct deliberately: every existing member is 4-byte-aligned
    // (int/float/int32_t), plus the one `bool` above that already costs the
    // compiler a 3-byte pad before `gearboxRatioNumerator`. Adding two
    // trailing uint8_t fields here uses 2 bytes and, at most, extends the
    // struct's own trailing pad (already required to round the struct size up
    // to its 4-byte alignment) - it does not move or reshuffle any existing
    // member's offset, which matters because this is a flash layout, not just
    // an in-memory one.
    uint8_t theme = THEME_DARK;
    uint8_t droDatum = DRO_DATUM_LEFT;

} LatheConfig;

// --- Flash layout pin -----------------------------------------------------
// This struct is blitted raw into flash (src/WebSettings.cpp), so its size and
// every member offset ARE the on-disk format. Anything that moves an existing
// offset silently reinterprets settings already saved on a user's machine -
// their leadscrew pitch read back as their jog speed, etc. - and nothing at
// runtime would report it. These asserts are compiled by BOTH the host test
// build and the device build, so such a change fails at compile time instead.
//
// Rules if you need to change this struct:
//   * Append new fields at the END only, and bump CHECKVALUE (above) in the
//     same commit so stored blobs shorter than the new struct are discarded.
//   * Then update the size assert below to the new size, and add offset
//     asserts for the new members. NEVER "fix" an existing offset assert to
//     match a moved member - that is the bug the assert exists to catch.
//   * The total must still fit the shared 4 KB sector alongside WebSettings -
//     see the sector asserts in src/WebSettings.cpp.
static_assert(sizeof(LatheConfig) == 44, "LatheConfig is a flash layout: size changed");
static_assert(alignof(LatheConfig) == 4, "LatheConfig alignment changed");
static_assert(offsetof(LatheConfig, check) == 0, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, spindleEncoderPpr) == 4, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, stepperPpr) == 8, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, invertDirection) == 12, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, gearboxRatioNumerator) == 16, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, gearboxRatioDenominator) == 20, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, leadscrewPitchMm) == 24, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, jogSpeed) == 28, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, leadscrewAcceleration) == 32, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, leadscrewMaxSpeed) == 36, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, theme) == 40, "LatheConfig flash offset moved");
static_assert(offsetof(LatheConfig, droDatum) == 41, "LatheConfig flash offset moved");

// An erased flash sector reads back as all-0xFF, and a zeroed one as all-0x00.
// CHECKVALUE must match neither, or a wiped/blank sector would be accepted as
// valid saved settings (see the boot check in src/main.cpp).
static_assert((uint32_t)CHECKVALUE != 0xFFFFFFFFu, "CHECKVALUE would match erased flash");
static_assert((uint32_t)CHECKVALUE != 0x00000000u, "CHECKVALUE would match blank flash");

// Maps a stored `droDatum` byte onto DroDatumPreference. Flash can hand back
// garbage (a short-read blob, or corruption) that is neither DRO_DATUM_LEFT
// nor DRO_DATUM_RIGHT - such a value must NOT be cast blindly into the enum
// (that would be undefined/nonsense for a strongly-typed enum class), so any
// value other than DRO_DATUM_RIGHT falls back to the safe default, Left.
DroDatumPreference toDroDatumPreference(uint8_t storedValue);

// Inverse mapping, for building the byte to persist.
uint8_t fromDroDatumPreference(DroDatumPreference pref);


class LatheConfigDerived {
private:
    LatheConfig* config;
    // Derived values are precomputed once in the constructor (config is fixed at
    // runtime - changes require a reboot) so the hot leadscrew loop reads cached
    // values instead of recomputing divisions every update().
    float m_leadscrewStepsPerMm;
    float m_jogSpeedPps;
    float m_leadscrewMaxSpeedPps;
    float m_accellerationPulseSec;
    float m_leadscrewInitialPulseDelay;
    float m_gearboxRatio;
    int m_dirRight;
    int m_dirLeft;
public:
    LatheConfigDerived(LatheConfig* config);

    // All accessors below are pure reads of the cached members (or, for the
    // first block, of the LatheConfig* they merely mirror) - const because
    // config is fixed at runtime (see the class comment above) and callers
    // like Leadscrew::getConfig() hand this out as a read-only view.
    int spindleEncoderPpr() const;
    int stepperPpr() const;
    bool invertDirection() const; // true => right = 1.
    int gearboxRatioNumerator() const;
    int gearboxRatioDenominator() const;
    float leadscrewPitchMm() const; //mm per turn
    int jogSpeed() const; //mm/s
    int leadscrewAcceleration() const; //mm/s2
    int leadscrewMaxSpeed() const; // mm/s
    uint8_t theme() const;
    DroDatumPreference droDatum() const;

    float leadscrewStepsPerMm() const;
    float jogSpeedPps() const;
    float leadscrewMaxSpeedPps() const;
    float accellerationPulseSec() const;
    float leadscrewInitialPulseDelay() const;
    float gearboxRatio() const;
    int dirRight() const;
    int dirLeft() const;

};

#endif