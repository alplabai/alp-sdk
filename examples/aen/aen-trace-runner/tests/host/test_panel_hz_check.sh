#!/bin/bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# tests/host/test_panel_hz_check.sh -- the refresh interlock of
# a32/release/build-release.sh (panel_hz_check.sh): a release HE whose display
# refreshes at anything but 30 Hz is refused, the override lets one through on
# purpose, and a 30 Hz HE passes. The refresh is derived from the build's
# generated zephyr.dts cdc200 node (panel_hz_of.py), not from a build option.
set -u
cd "$(dirname "$0")/../.." || exit 1
t=$(mktemp -d "${TMPDIR:-/tmp}/tr-panel-hz-check.XXXXXX")
trap 'rm -rf "$t"' EXIT
fail=0
# shellcheck source=a32/release/panel_hz_check.sh
source a32/release/panel_hz_check.sh

mk_build() { # dir pclk vfront ("" pclk: no cdc200 node at all)
	mkdir -p "$1/zephyr"
	if [ -n "${2:-}" ]; then
		cat > "$1/zephyr/zephyr.dts" <<DTS
/ {
	soc {
		cdc200: cdc200@49031000 {
			width = < 0x2d0 >;
			height = < 0x500 >;
			hsync-len = < 0x6 >;
			hfront-porch = < 0xc >;
			hback-porch = < 0x18 >;
			vsync-len = < 0x2 >;
			vfront-porch = < $3 >;
			vback-porch = < 0xe >;
			clock-frequency = < $2 >;
		};
	};
};
DTS
	else
		echo '/ { };' > "$1/zephyr/zephyr.dts"
	fi
}
# RK055: 40,000,000 Hz over 762 x (1280 + 2 + 16 + 14): 40 Hz; with the 30 Hz
# overlay's 454-line front porch (0x1c6): 30 Hz.
mk_build "$t/rk40" 0x2625a00 0x10
mk_build "$t/rk30" 0x2625a00 0x1c6
mk_build "$t/nonode" ""
mkdir -p "$t/nodts"

expect() { # want(0|1) why dir
	local want=$1 why=$2 out rc
	out=$(panel_hz_check "$3" 2>&1)
	rc=$?
	if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && { [ $rc -eq 0 ] || ! grep -q REFUSED <<<"$out"; }; }; then
		echo "FAIL panel_hz_check: $why (rc=$rc): $out"
		fail=1
	fi
}
expect 0 "a display refreshing at 30 Hz (the release config)" "$t/rk30"
expect 1 "a 40 Hz display (the stock RK055 timing)" "$t/rk40"
expect 1 "a zephyr.dts with no cdc200 node" "$t/nonode"
expect 1 "no zephyr.dts at all" "$t/nodts"

# The override is the one deliberate way to release a non-30 Hz build.
for d in rk40 nonode nodts; do
	out=$(TR_ALLOW_PANEL_HZ_40=ON panel_hz_check "$t/$d" 2>&1)
	rc=$?
	if [ $rc -ne 0 ] || grep -q REFUSED <<<"$out"; then
		echo "FAIL panel_hz_check: TR_ALLOW_PANEL_HZ_40=ON did not override $d (rc=$rc): $out"
		fail=1
	fi
done

[ $fail = 0 ] && echo "test_panel_hz_check: ok"
exit $fail
