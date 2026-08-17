
#include <stdint.h>
#include "latheconfig.h"
#ifndef WebSettings_h
#define WebSettings_h


typedef struct WebSettings {
    int32_t check;
    char  ssid[32];
    char  password[63];
    char  url[512];
} WebSettings;

WebSettings* getWebSettings();
LatheConfig* getLatheSettings();

// --- Device-writable preferences ------------------------------------------
//
// LATHE GEOMETRY IS WEB-ONLY AND IS NEVER WRITABLE FROM THE DEVICE.
// spindleEncoderPpr, stepperPpr, invertDirection, the gearbox ratio,
// leadscrewPitchMm, jogSpeed, leadscrewAcceleration and leadscrewMaxSpeed are
// commissioning values: set once over Wi-Fi with the lathe offline, and
// changed only there. `theme` and `droDatum` are the only two fields the
// device itself may write (docs/ux-redesign.md section 6 "Saving settings from
// the device").
//
// That is why these two functions take VALUES rather than a LatheConfig*. The
// predecessor, saveLatheSettings(LatheConfig*), wrote sizeof(LatheConfig) from
// whatever struct the caller handed it, so a theme toggle rewrote all nine
// geometry fields out of an in-RAM reconstruction - and if that reconstruction
// was ever stale, incomplete (a newly added field the seeding code missed) or
// simply never seeded (a null leadscrew left it holding compiled-in defaults),
// the first save silently replaced the user's commissioned geometry with it.
// With no LatheConfig in the signature there is no such struct to be wrong:
// geometry only ever exists inside saveLathePreferences(), as bytes it has
// just read back out of flash, and it cannot be passed in from outside.
//
// Neither function is host-testable: WebSettings.cpp only builds for the esp32
// envs (ESP.flashRead/flashEraseSector/flashWrite have no native stub). The
// part that decides WHICH bytes change is factored into lib/config
// (applyLathePreferences() and friends) precisely so that it can be, and is,
// covered by test/test_latheconfig. The flash side must be verified on device.

// Reads back the two device-writable preferences currently stored in flash.
// Returns false - with `theme`/`droDatum` set to the safe defaults - if flash
// does not hold a blob this firmware recognises (check != CHECKVALUE), which
// at runtime should be unreachable: main.cpp drops the device into AP setup
// instead of starting the machine when the stored blob fails that same test.
//
// This, not any in-RAM copy, is where a menu tile gets the value it is about
// to toggle. Callers must not cache it: the point of re-reading is that flash
// is the authority for what is persisted.
bool readLathePreferences(uint8_t& theme, DroDatumPreference& droDatum);

// Persists both preferences, as a read-modify-write of the stored settings.
// It re-reads the stored WebSettings AND the stored LatheConfig, overwrites
// only `theme` and `droDatum` in the latter, then erases the shared sector and
// writes both back:
//
//   * The LatheConfig re-read is what carries the user's geometry through. It
//     comes from flash, never from RAM, so it cannot be corrupted by a stale
//     or incomplete copy - see above.
//   * The WebSettings re-read is what preserves the Wi-Fi credentials and OTA
//     URL. Both structs live in ONE 4 KB flash sector that is erased as a unit
//     (NVM_Offset/address/latheaddress in WebSettings.cpp), so writing the
//     LatheConfig alone would wipe the saved SSID/password/URL as a side
//     effect of a theme toggle.
//
// Returns false without writing anything if the stored blob is unrecognised
// (see readLathePreferences() - rewriting it would bless unknown bytes as
// valid geometry) or if the carriage is under power.
//
// REFUSES TO SAVE WHILE THE CARRIAGE IS UNDER POWER. A flash erase takes the
// flash lock and disables the instruction cache on BOTH cores; nothing in
// main.cpp or lib/leadscrew is IRAM_ATTR, so the SpindleTask stalls for the
// whole operation - tens of milliseconds for a 4 KB sector erase - and step
// generation stops dead. That is harmless for setValues(), which runs solely
// in AP config mode where motion never runs, but this one is called from the
// running machine's menu, where a mid-cut save would lose spindle sync.
//
// The guard lives here rather than in the caller because a caller-side rule is
// one someone eventually forgets, and the failure is silent and expensive.
// Callers should surface the false return as "stop the machine first", and
// must apply the new value to the UI only AFTER a true return - otherwise the
// screen shows a setting the machine did not keep, which reverts on the next
// boot.
bool saveLathePreferences(uint8_t theme, DroDatumPreference droDatum);

void startWebServer() ;

void wifiLoop() ;

#endif