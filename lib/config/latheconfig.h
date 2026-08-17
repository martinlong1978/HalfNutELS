#include <stdint.h>

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

    int spindleEncoderPpr();
    int stepperPpr();
    bool invertDirection(); // true => right = 1. 
    int gearboxRatioNumerator();
    int gearboxRatioDenominator();
    float leadscrewPitchMm(); //mm per turn
    int jogSpeed(); //mm/s
    int leadscrewAcceleration(); //mm/s2
    int leadscrewMaxSpeed(); // mm/s
    uint8_t theme();
    DroDatumPreference droDatum();

    float leadscrewStepsPerMm();
    float jogSpeedPps();
    float leadscrewMaxSpeedPps();
    float accellerationPulseSec();
    float leadscrewInitialPulseDelay();
    float gearboxRatio();
    int dirRight();
    int dirLeft();

};

#endif