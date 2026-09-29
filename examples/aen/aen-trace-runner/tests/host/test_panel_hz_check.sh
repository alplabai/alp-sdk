#!/bin/bash
# tests/host/test_panel_hz_check.sh -- the TR_PANEL_HZ interlock of
# a32/release/build-release.sh (panel_hz_check.sh): a release HE not
# explicitly built with -DTR_PANEL_HZ=30 is refused, the override lets one
# through on purpose, and a correctly-configured HE passes.
set -u
cd "$(dirname "$0")/../.." || exit 1
t=$(mktemp -d "${TMPDIR:-/tmp}/tr-panel-hz-check.XXXXXX")
trap 'rm -rf "$t"' EXIT
fail=0
# shellcheck source=a32/release/panel_hz_check.sh
source a32/release/panel_hz_check.sh

mk_build() { # dir cache_line
	mkdir -p "$1"
	[ -n "${2:-}" ] && echo "$2" > "$1/CMakeCache.txt"
}
mk_build "$t/good" 'TR_PANEL_HZ:STRING=30'
mk_build "$t/hz40" 'TR_PANEL_HZ:STRING=40'
mk_build "$t/no-cache" ''

expect() { # want(0|1) why dir
	local want=$1 why=$2 out rc
	out=$(panel_hz_check "$3" 2>&1)
	rc=$?
	if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && { [ $rc -eq 0 ] || ! grep -q REFUSED <<<"$out"; }; }; then
		echo "FAIL panel_hz_check: $why (rc=$rc): $out"
		fail=1
	fi
}
expect 0 "TR_PANEL_HZ:STRING=30 -- the release config" "$t/good"
expect 1 "TR_PANEL_HZ:STRING=40 -- CMake's own default, not this release's" "$t/hz40"
expect 1 "no CMakeCache.txt at all" "$t/no-cache"

# fix round 15 (orchestrator finding): CMake's default is 40, silently, if
# -DTR_PANEL_HZ=30 is never passed -- round 14's own release builds hit
# exactly this. TR_ALLOW_PANEL_HZ_40=ON is the one deliberate way around it.
out=$(TR_ALLOW_PANEL_HZ_40=ON panel_hz_check "$t/hz40" 2>&1)
rc=$?
if [ $rc -ne 0 ] || grep -q REFUSED <<<"$out"; then
	echo "FAIL panel_hz_check: TR_ALLOW_PANEL_HZ_40=ON did not override a 40 Hz build (rc=$rc): $out"
	fail=1
fi
out=$(TR_ALLOW_PANEL_HZ_40=ON panel_hz_check "$t/no-cache" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
	echo "FAIL panel_hz_check: TR_ALLOW_PANEL_HZ_40=ON did not override a missing CMakeCache.txt (rc=$rc): $out"
	fail=1
fi

[ $fail = 0 ] && echo "test_panel_hz_check: ok"
exit $fail
