#ifndef ELS_VERSION_H
#define ELS_VERSION_H

// Firmware version. Must match the git tag used for the GitHub release
// (leading "v"), because the OTA version check compares this string against the
// release's `tag_name` verbatim. Bump this before publishing a new release.
#define FIRMWARE_VERSION "v1.0.5"

// ---------------------------------------------------------------------------
// BUILD PROVENANCE (GitHub issue #4)
//
// The three ELS_BUILD_* macros are injected by scripts/build_provenance.py,
// which is a pre-script on the two esp32 envs. They are derived from git, never
// typed, because the failure this guards against IS someone forgetting: during
// the Aug 2026 EP2 filming the deliberately-broken demo branch and master both
// reported v1.0.1, and the About screen could not tell them apart.
//
// The defaults below are what the `native` env and the screenshot harness use -
// neither runs the script, and both want output that does not change with
// whatever happens to be checked out.
// ---------------------------------------------------------------------------
#ifndef ELS_BUILD_IS_RELEASE
#define ELS_BUILD_IS_RELEASE 1
#endif
#ifndef ELS_BUILD_SHA
#define ELS_BUILD_SHA "hosted"
#endif
#ifndef ELS_BUILD_SUFFIX
#define ELS_BUILD_SUFFIX ""
#endif

// What the SPLASH shows: the version, plus "-<branch>@<sha>" when this is not a
// clean master build. 14pt across the full screen width, so there is room.
#define FIRMWARE_VERSION_DISPLAY FIRMWARE_VERSION ELS_BUILD_SUFFIX

// What the ABOUT screen's big FIRMWARE value shows. On a release that is the
// version; on anything else it is the SHA INSTEAD, because a SHA is
// self-evidently not a release and answers "which build is this?" directly.
// Both fit ABOUT_FW_W, which is sized for ten characters at 26pt.
#if ELS_BUILD_IS_RELEASE
#define FIRMWARE_VERSION_ABOUT FIRMWARE_VERSION
#else
#define FIRMWARE_VERSION_ABOUT ELS_BUILD_SHA
#endif

#endif
