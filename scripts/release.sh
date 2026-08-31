#!/usr/bin/env bash
#
# Publish the current esp32dev_publish build as a GitHub release, so the ELS OTA
# updater can pull it from the "latest" release permalink:
#   https://github.com/<repo>/releases/latest/download/elstft.bin
#
# Prerequisites:
#   1. gh CLI installed and authenticated (`gh auth login`).
#   2. A completed release build, e.g.:
#        ~/.platformio/penv/Scripts/pio.exe run -e esp32dev_publish
#
# The release tag is taken from FIRMWARE_VERSION in include/version.h, so bump
# that (and rebuild) before publishing a new version.
#
# NOTE: this replaces the old scp-to-web-host publish step (publish.cmd).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_H="$REPO_ROOT/include/version.h"
BUILD_DIR="$REPO_ROOT/.pio/build/esp32dev_publish"
FIRMWARE="$BUILD_DIR/firmware.bin"
# The OTA permalink downloads an asset literally named elstft.bin, so the upload
# must have that filename. Copy the build output to that name before uploading.
ASSET="$BUILD_DIR/elstft.bin"

# Pull FIRMWARE_VERSION ("v0.1.0") out of version.h.
VERSION="$(grep -oE '#define[[:space:]]+FIRMWARE_VERSION[[:space:]]+"[^"]+"' "$VERSION_H" \
  | grep -oE '"[^"]+"' | tr -d '"')"

if [ -z "${VERSION:-}" ]; then
  echo "ERROR: could not read FIRMWARE_VERSION from $VERSION_H" >&2
  exit 1
fi

if [ ! -f "$FIRMWARE" ]; then
  echo "ERROR: firmware not found at $FIRMWARE" >&2
  echo "Build it first:  pio run -e esp32dev_publish" >&2
  exit 1
fi

# THE GUARD THAT MATTERS. When CI runs this from a tag push, the tag and
# FIRMWARE_VERSION must agree, because the device compares the release's
# tag_name against FIRMWARE_VERSION VERBATIM (src/ESPCommsManager.cpp). If they
# disagree the update is silently broken in one of two directions: a device on
# the new firmware keeps being offered it, or one on the old firmware is never
# told. Neither surfaces until someone tries to update a real lathe.
if [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then
  TAG="${GITHUB_REF_NAME:-}"
  if [ "$TAG" != "$VERSION" ]; then
    echo "ERROR: tag $TAG does not match FIRMWARE_VERSION $VERSION." >&2
    echo "       The OTA check compares these verbatim, so publishing this" >&2
    echo "       would break the update path. Bump include/version.h to match" >&2
    echo "       the tag (or retag), then push again." >&2
    exit 1
  fi
fi

cp "$FIRMWARE" "$ASSET"

echo "Publishing GitHub release $VERSION (asset: elstft.bin)"

# In CI the notes are generated from the commits since the last release;
# locally there is no such history to lean on without more ceremony than a
# bench publish wants, so it keeps the one-liner it always had. Either can be
# rewritten afterwards with `gh release edit --notes`.
if [ -n "${GITHUB_ACTIONS:-}" ]; then
  gh release create "$VERSION" \
    "$ASSET" \
    --title "$VERSION" \
    --generate-notes
else
  gh release create "$VERSION" \
    "$ASSET" \
    --title "$VERSION" \
    --notes "Firmware release $VERSION"
fi

echo "Done. OTA permalink:"
echo "  https://github.com/martinlong1978/HalfNutELS/releases/latest/download/elstft.bin"
