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
// BUMPED AGAIN for the debug-capture sink URL added to WebSettings
// (src/WebSettings.h). That struct grew by 256 bytes, and LatheConfig is stored
// immediately after it - `latheaddress = address + sizeof(WebSettings)` in
// src/WebSettings.cpp - so every existing device's stored LatheConfig is now at
// the wrong offset and would read back as 44 bytes of the middle of the OTA URL
// string. Bumping this makes `check != CHECKVALUE` on both structs, so the
// device discards the whole blob and boots into first-run AP setup.
//
// CONSEQUENCE, and it is not small: THIS WIPES THE SAVED WI-FI CREDENTIALS AND
// EVERY COMMISSIONED LATHE MEASUREMENT. Both structs share one 4 KB sector that
// is erased as a unit. After flashing this firmware the machine comes up as the
// ELS_Wifi access point and has to be set up from scratch - SSID, password, OTA
// URL, encoder PPR, stepper PPR, gearbox ratio, leadscrew pitch, direction, jog
// speed, acceleration and max speed. Write them down before flashing.
#define CHECKVALUE 0xDFEB0942

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
//
// KNOWN BLIND SPOT in the size assert, which is why the offset asserts below
// are the load-bearing ones: the struct currently ends in two bytes of trailing
// padding (theme and droDatum occupy 40 and 41 of 44), so up to two more
// uint8_t fields can be appended with sizeof(LatheConfig) unchanged and this
// assert still passing. It catches a RESHAPE, not an addition. src/buttonpad.cpp
// used to carry a copy of this assert as its guard that a field-by-field copy of
// the struct was still complete; that guard was unsound for exactly this reason,
// and it is gone along with the copy - device-side saves now carry geometry
// through from flash and never enumerate the fields at all.
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

// --- Device-writable preferences ------------------------------------------
// `theme` and `droDatum` are the ONLY two LatheConfig fields the device itself
// may write (docs/ux-redesign.md section 6). Everything else in the struct is
// lathe geometry: commissioning values, set once over Wi-Fi with the machine
// offline, and never writable from the keypad. The helpers below are the whole
// vocabulary the device side needs - normalise, toggle, and apply-onto-a-
// stored-blob - so that no device-side code ever has to build a LatheConfig of
// its own (which is what used to let a theme toggle rewrite geometry from a
// stale in-RAM copy).
//
// They live here, in lib/config, rather than in src/WebSettings.cpp so they
// are host-testable: src/ is not built for the native env, so the flash side
// of the save cannot be tested, but this - the part that decides which bytes
// change - can be, and is (test/test_latheconfig).

// Normalises a stored `theme` byte, on exactly the same reasoning as
// toDroDatumPreference() above: flash can hold any byte, and only THEME_LIGHT
// means light. Anything else - garbage, a short-read blob, a theme id written
// by some future firmware - resolves to the safe default, THEME_DARK. Returned
// as uint8_t rather than an enum because Display::setTheme() takes the raw
// stored byte.
uint8_t normaliseTheme(uint8_t storedValue);

// The Theme tile's action: normalise, then flip. Garbage in therefore gives
// THEME_LIGHT out (garbage normalises to dark, and dark toggles to light),
// which is a defined result rather than a coin toss.
uint8_t toggleTheme(uint8_t storedValue);

// The DRO datum tile's action. Takes and returns the enum, not the stored
// byte, so an unnormalised value cannot reach it in the first place.
DroDatumPreference toggleDroDatum(DroDatumPreference current);

// Overwrites ONLY the two device-writable preference bytes of `stored`, in
// their normalised form. Every geometry field - and `check` - is left exactly
// as the caller found it.
//
// `stored` is meant to be a LatheConfig that was JUST read back from flash, so
// that a device-side save carries the user's commissioned geometry through
// from flash rather than from any in-RAM reconstruction of it. Deliberately
// does not stamp `check`: blessing a blob this firmware did not recognise
// would promote unknown bytes to "valid geometry", which is precisely the
// failure this whole path exists to prevent. The caller decides whether the
// blob was valid before it ever gets here (saveLathePreferences(),
// src/WebSettings.cpp).
void applyLathePreferences(LatheConfig& stored, uint8_t theme,
                           DroDatumPreference droDatum);


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

    // BOOT-TIME values only. Unlike everything above them, these two are
    // device-writable (the Theme / DRO datum menu tiles), and a device-side
    // save writes FLASH - it does not write back through this view, which is
    // read-only by design and, more to the point, must stay read-only: a
    // general setter here would let a caller change leadscrewPitchMm and leave
    // every precomputed value below stale. So after a toggle these accessors
    // still report what was stored at boot.
    //
    // That is fine for their one legitimate use - seeding the live value at
    // construction time (Display's constructor) - and wrong for anything else.
    // If you need the CURRENT theme or datum at runtime, read it from whoever
    // owns the live value (Display::m_palette / Display::m_droDatum), or, if
    // you need the persisted one, from flash via readLathePreferences()
    // (src/WebSettings.h). Do not add a runtime reader here.
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