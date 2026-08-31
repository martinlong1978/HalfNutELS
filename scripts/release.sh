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
BUILD_DIR="$REPO_ROOT/.pio/build/esp32dev_publish"
FIRMWARE="$BUILD_DIR/firmware.bin"
# The OTA permalink downloads an asset literally named elstft.bin, so the upload
# must have that filename. Copy the build output to that name before uploading.
ASSET="$BUILD_DIR/elstft.bin"

# THE VERSION IS THE TAG. It is no longer read from include/version.h, which
# no longer holds one: scripts/build_provenance.py injects FIRMWARE_VERSION
# into the build from this same tag, so the string the device reports and the
# string the release is named after come from ONE place and cannot disagree.
# That removes the failure this script used to have to guard against.
if [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then
  VERSION="${GITHUB_REF_NAME:-}"
else
  # A bench publish. HEAD must be exactly on a tag - without one there is
  # nothing to name the release after, and whatever got built was stamped with
  # the PREVIOUS tag, so publishing it would be a lie.
  VERSION="$(git -C "$REPO_ROOT" describe --tags --exact-match 2>/dev/null || true)"
  if [ -z "$VERSION" ]; then
    echo "ERROR: HEAD is not on a tag, so there is no version to publish." >&2
    echo "       Tag the commit first:  git tag v1.0.6 && git push <remote> v1.0.6" >&2
    echo "       (pushing the tag is normally enough - CI publishes it for you)" >&2
    exit 1
  fi
fi

if [ ! -f "$FIRMWARE" ]; then
  echo "ERROR: firmware not found at $FIRMWARE" >&2
  echo "Build it first:  pio run -e esp32dev_publish" >&2
  exit 1
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
