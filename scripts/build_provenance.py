"""Stamp the running firmware with where it actually came from. GitHub issue #4.

WHY THIS EXISTS. `FIRMWARE_VERSION` in include/version.h is a hand-edited
string on a branch, so a build from an experimental branch reports whatever
that branch's version.h happens to say. During the Aug 2026 EP2 filming,
`demo/ep2-pre-sync-fix` - which deliberately reproduces the pre-c3db8cd naive
sync-start and over-speeds at the start of a cut - and the then-current master
build BOTH reported v1.0.1. When the lathe began stopping mid-cut, the About
screen could not distinguish "running the deliberately broken demo firmware"
from "running good firmware with a new bug", and hours went into chasing a
motion fault that was very likely the demo branch working as designed.

So this is derived from the build, never typed. It cannot be forgotten,
because forgetting it is the failure mode it exists to prevent.

WHAT IT DEFINES

  ELS_BUILD_IS_RELEASE  1 only for a clean tree on master. 0 for anything else.
  ELS_BUILD_SHA         short commit, plus "*" when the tree is dirty. This is
                        what the About screen shows INSTEAD of the version on a
                        non-release build - a SHA is self-evidently not a
                        release, and at 26pt it fits the same box a version
                        does (ABOUT_FW_W holds ten characters).
  ELS_BUILD_SUFFIX      "" on a release, else "-<branch>@<sha>" for the splash,
                        which has a full screen width at 14pt to spend.

WHAT IT DELIBERATELY DOES NOT TOUCH. `FIRMWARE_VERSION` itself. The OTA check
compares the release `tag_name` against it verbatim (src/ESPCommsManager.cpp),
so a suffix leaking in there would break the version-skip. A non-release build
simply never matches a tag, which is the right outcome anyway.

Not registered for the `native` env: host tests and the screenshot harness take
the defaults in version.h, so their output stays deterministic and does not
churn with the checkout.
"""

import os
import subprocess

Import("env")


def _git(*args):
    """Return stripped stdout, or None if git is unavailable or fails."""
    try:
        out = subprocess.run(
            ["git"] + list(args),
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip()


# GitHub Actions checks out a DETACHED HEAD, so `rev-parse --abbrev-ref HEAD`
# answers "HEAD" and the branch test below could never pass - every CI build,
# including the one that produces the released binary, would stamp itself "not
# a release" and the About screen would show a SHA where the version belongs.
#
# The CI context is authoritative there and is not a thing anyone types, so it
# is trusted in preference to the local git heuristic:
#   GITHUB_REF_TYPE=tag     -> a release build, by definition. Releases are cut
#                              by pushing a tag, and the runner's checkout is
#                              clean by construction.
#   GITHUB_REF_TYPE=branch  -> GITHUB_REF_NAME is the real branch name; from
#                              there the ordinary rule applies unchanged.
ci_ref_type = os.environ.get("GITHUB_REF_TYPE")
ci_ref_name = os.environ.get("GITHUB_REF_NAME")

branch = _git("rev-parse", "--abbrev-ref", "HEAD")
sha = _git("rev-parse", "--short", "HEAD")

if ci_ref_type == "branch" and ci_ref_name:
    branch = ci_ref_name

# `git status --porcelain` is empty exactly when the tree is clean. An untracked
# file counts: it may well be the thing that changed behaviour.
status = _git("status", "--porcelain")
dirty = bool(status)

if ci_ref_type == "tag" and ci_ref_name:
    # A tag build. Say so, and carry the tag rather than a branch name.
    is_release = True
    sha_label = sha if sha else ci_ref_name
    suffix = ""
elif sha is None or branch is None:
    # No git, or not a checkout - a tarball build, say. Say so rather than
    # silently claiming to be a release, which is the whole point of the file.
    is_release = False
    sha_label = "nogit"
    suffix = "-nogit"
else:
    is_release = (branch == "master") and not dirty
    sha_label = sha + ("*" if dirty else "")
    if is_release:
        suffix = ""
    else:
        # Keep the branch short enough that the splash line stays on one row:
        # 14pt across 320px is comfortable to about 40 characters all told.
        short_branch = branch if len(branch) <= 16 else branch[:15] + "~"
        suffix = "-{}@{}".format(short_branch, sha_label)

env.Append(
    CPPDEFINES=[
        ("ELS_BUILD_IS_RELEASE", "1" if is_release else "0"),
        ("ELS_BUILD_SHA", env.StringifyMacro(sha_label)),
        ("ELS_BUILD_SUFFIX", env.StringifyMacro(suffix)),
    ]
)

print(
    "build provenance: {} (branch={} sha={} dirty={})".format(
        "RELEASE" if is_release else "not a release", branch, sha, dirty
    )
)
