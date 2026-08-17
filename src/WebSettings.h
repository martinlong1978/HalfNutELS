
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

// Persist lathe settings written from the DEVICE itself (theme, DRO datum -
// docs/ux-redesign.md section 6 "Saving settings from the device"), as
// opposed to setValues() which persists settings posted from the web setup
// form. MUST be implemented as a read-modify-write of the shared sector:
// WebSettings and LatheConfig are memcpy'd into ONE 4 KB flash sector that is
// erased as a unit (see NVM_Offset/address/latheaddress in WebSettings.cpp),
// so writing *config without first re-reading the current WebSettings (via
// getWebSettings()) and rewriting them alongside it would erase the saved
// Wi-Fi SSID/password/OTA URL as a side effect of a theme toggle.
//
// Not implemented here - and not host-testable, since WebSettings.cpp only
// builds for the esp32 envs (ESP.flashRead/flashEraseSector/flashWrite have
// no native stub). Verify on device.
void saveLatheSettings(LatheConfig* config);

void startWebServer() ;

void wifiLoop() ;

#endif