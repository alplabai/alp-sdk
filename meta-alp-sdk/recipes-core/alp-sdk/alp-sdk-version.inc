# Single PV for every recipe that builds from this repo (#2827, #2850), so a
# package feed offers a newer version of all of them when a release ships.
# It cannot be read from metadata/sdk_version.yaml here: the source is only
# fetched after parse.  scripts/bump_version.py rewrites this line and
# scripts/check_version_doc_sync.py fails on drift.  A SemVer pre-release
# uses "~" (0.17.0~rc2 sorts BEFORE 0.17.0 under opkg/dpkg/rpm).
PV      = "0.17.0"
