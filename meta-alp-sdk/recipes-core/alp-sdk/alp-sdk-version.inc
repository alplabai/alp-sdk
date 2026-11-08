# Single PV for every recipe that builds from this repo (#2827, #2850), so a
# package feed offers a newer version of all of them when a release ships.
# It cannot be read from metadata/sdk_version.yaml here: the source is only
# fetched after parse.  scripts/bump_version.py rewrites this line and
# scripts/check_version_doc_sync.py fails on drift.  A SemVer pre-release
# uses "~" (0.17.0~rc2 sorts BEFORE 0.17.0 under opkg/dpkg/rpm).
PV      = "0.18.0"

# One pinned SRCREV for every recipe that builds from this repo, so an image
# rebuilds bit-for-bit and libalp_sdk, libalp_chips and the examples can never
# come from different commits.  Always a commit on branch=main (every
# SRC_URI names it); never ${AUTOREV}, and never branch=dev.  This is the
# commit the vPV tag points at.  Bump it with PV, see "Bumping the alp-sdk
# pin" in meta-alp-sdk/README.md (tests/scripts/test_yocto_srcrev_pin.py
# fails on AUTOREV, a dev branch or a malformed pin).
SRCREV  = "bce3c67ab78d7dda47b1ffa9ca893f34751124c3"
