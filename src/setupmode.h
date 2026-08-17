// setupmode.h — runtime entry point into Wi-Fi/config (AP) mode.
//
// Exposed for future UI code (a "Setup" menu tile) to call. Not wired to any
// UI yet — see docs/ux-redesign.md section 6 ("Setup tile needs a reboot
// flag"). Calling this reboots the device; on the next boot, main.cpp's
// setup() will detect the request (via RTC memory, see main.cpp) and enter
// config/AP mode instead of normal operation.
#pragma once

void requestSetupOnNextBoot();
