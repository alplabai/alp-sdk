# shellcheck shell=bash
# a32/release/panel_hz_check.sh -- sourced by build-release.sh (and
# tests/host/test_panel_hz_check.sh). panel_hz_check HE_BUILD_DIR: 0 when the
# display the HE was built for refreshes at 30 Hz (or the override is set),
# else prints a loud refusal and returns 1.
#
# The release's game pace, FLASH-RECIPE.md and the on-glass 30 fps story all
# assume a 30 Hz refresh. The refresh is whatever the build's display
# devicetree says (the shield's cdc200 timings, plus panel_30hz.overlay for the
# RK055): a release HE built for the stock RK055 timing is 40 Hz, and an A32
# renderer paced for 33 ms frames then runs at 25 ms -- round 14's release
# builds hit exactly that and the attract HUD read a genuine 37-41 fps on real
# silicon. So the check reads the REFRESH the HE really has, from its
# generated zephyr.dts (panel_hz_of.py: pclk / (htotal * vtotal), the same
# arithmetic game/panel_hz.h does), not a build option.

panel_hz_refuse() {
	{
		echo "######################################################################"
		echo "build-release: REFUSED -- $*"
		echo "  The release is paced for a 30 Hz panel. The stock RK055 shield timing"
		echo "  is 40 Hz: add panel_30hz.overlay (-DEXTRA_DTC_OVERLAY_FILE=...), or use"
		echo "  a shield that refreshes at 30 Hz (the Riverdi RVT121). Set"
		echo "  TR_ALLOW_PANEL_HZ_40=ON to release a genuine 40 Hz build on purpose."
		echo "######################################################################"
	} >&2
}

panel_hz_check() {
	local hed=$1 here hz
	here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
	if [ "${TR_ALLOW_PANEL_HZ_40:-OFF}" = ON ]; then
		echo "build-release: TR_ALLOW_PANEL_HZ_40=ON -- allowing $hed without checking the refresh" >&2
		return 0
	fi
	if [ ! -f "$hed/zephyr/zephyr.dts" ]; then
		panel_hz_refuse "$hed/zephyr/zephyr.dts missing -- can't confirm the display refresh"
		return 1
	fi
	if ! hz=$(python3 "$here/panel_hz_of.py" "$hed/zephyr/zephyr.dts"); then
		panel_hz_refuse "$hed has no cdc200 display timings in zephyr.dts -- can't confirm the refresh"
		return 1
	fi
	if [ "$hz" != 30 ]; then
		panel_hz_refuse "$hed's display refreshes at $hz Hz"
		return 1
	fi
	return 0
}
