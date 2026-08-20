#ifndef ELS_VERSION_H
#define ELS_VERSION_H

// Firmware version. Must match the git tag used for the GitHub release
// (leading "v"), because the OTA version check compares this string against the
// release's `tag_name` verbatim. Bump this before publishing a new release.
#define FIRMWARE_VERSION "v0.3.0"

#endif
