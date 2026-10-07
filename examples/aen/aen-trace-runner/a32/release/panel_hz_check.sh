# shellcheck shell=bash
# a32/release/panel_hz_check.sh -- sourced by build-release.sh (and
# tests/host/test_panel_hz_check.sh). panel_hz_check HE_BUILD_DIR: 0 when
# the HE was configured with -DTR_PANEL_HZ=30 (or the override is set), else
# prints a loud refusal and returns 1.
#
# fix round 15 (silicon finding, orchestrator): CMakeLists.txt's own
# `set(TR_PANEL_HZ 40 CACHE STRING ...)` default is 40 -- a release HE built
# without an EXPLICIT -DTR_PANEL_HZ=30 silently links the shield's native
# 40 Hz timing while every other release assumption (game pace, FLASH-
# RECIPE.md, the "30.0 Hz with TR_PANEL_HZ=30" story in src/main.c's own
# header comment) says 30. Round 14's own HE builds (build15, build16) hit
# exactly this: no -DTR_PANEL_HZ=30 passed, CMake's default silently took
# over, and the attract-mode HUD read a genuine 37-41 fps on real silicon --
# not a game-logic bug (the flip path was traced clean, see that round's own
# report), just the true native 40 Hz panel rate coming through unchecked.

panel_hz_refuse() {
	{
		echo "######################################################################"
		echo "build-release: REFUSED -- $*"
		echo "  CMakeLists.txt's TR_PANEL_HZ default is 40 (the shield's native"
		echo "  timing), not the release's intended 30 -- pass -DTR_PANEL_HZ=30"
		echo "  explicitly. Set TR_ALLOW_PANEL_HZ_40=ON to release a genuine 40 Hz"
		echo "  build on purpose."
		echo "######################################################################"
	} >&2
}

panel_hz_check() {
	local hed=$1
	if [ "${TR_ALLOW_PANEL_HZ_40:-OFF}" = ON ]; then
		echo "build-release: TR_ALLOW_PANEL_HZ_40=ON -- allowing $hed without checking TR_PANEL_HZ" >&2
		return 0
	fi
	if [ ! -f "$hed/CMakeCache.txt" ]; then
		panel_hz_refuse "$hed/CMakeCache.txt missing -- can't confirm TR_PANEL_HZ"
		return 1
	fi
	if ! grep -q '^TR_PANEL_HZ:STRING=30$' "$hed/CMakeCache.txt"; then
		panel_hz_refuse "$hed was not configured with -DTR_PANEL_HZ=30"
		return 1
	fi
	return 0
}
