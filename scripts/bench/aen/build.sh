#!/usr/bin/env bash
# scripts/bench/aen/build.sh <app-src-dir> [extra -D args...]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh).
# Runs under WSL2 on Windows; the west build itself is cross-platform
# but this wrapper assumes a POSIX shell. See docs/aen-bench-bringup.md
# and scripts/bench/aen/README.md.
#
# Pristine-build an AEN bench app for the E8 M55-HE target.
# Overlays auto-apply: this builds the fully-qualified $AEN_BOARD target
# (default alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he, alp-sdk#2094), so
# Zephyr picks up a boards/ overlay/conf qualified by either the FULL stem
# (alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he) or the SHORT stem
# (alp_e1m_aen803_m55_he_rtss_he, dropping the SoC id) and app.overlay by
# name automatically -- no explicit -DEXTRA_DTC_OVERLAY_FILE force needed
# (the examples ship qualified names, not the bare board name that would
# silently drop). Most AEN examples have not yet grown an AEN803-qualified
# overlay/conf (alp-sdk#2101); this script REFUSES to build an app whose
# boards/ ships a qualified .overlay or .conf for a different board than
# $AEN_BOARD resolves to (checked independently per kind), rather than
# silently dropping it (alp-sdk#2094) -- override AEN_BOARD for such an
# app, e.g.:
#   AEN_BOARD=alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he ./build.sh <app>
# For a Flow C RAM-run, pass the bench-only ITCM retarget explicitly -- it is
# NOT one of the auto-applied overlays above (it lives outside the app, on
# purpose: the retarget is a bench concern, not something any app's own
# prj.conf/overlay should carry) -- both halves together, e.g.:
#   -DEXTRA_CONF_FILE="scripts/bench/aen/aen-bench-shared.conf;scripts/bench/aen/aen-flowc-itcm.conf" \
#   -DEXTRA_DTC_OVERLAY_FILE="scripts/bench/aen/aen-flowc-itcm.overlay"
# See docs/aen-bench-bringup.md, Flow C. Prints errors + the memory-region
# summary only.
set -e

# A pure `west build` wrapper -- never touches SE_UART or a J-Link probe --
# so an operator's LG_PLACE (exported for OTHER helpers in the same shell)
# must not abort a plain compile on a labgrid/reservation problem that has
# nothing to do with building (alp-sdk#2064 review).
BENCH_ENV_NO_PROBE=1
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

APP="$1"
shift || true
BOARD="$AEN_BOARD"
NAME=$(basename "$APP")
BD="$BENCH_ROOT/build/$NAME"

if [ -z "${HAL_ALIF_DIR:-}" ]; then
	echo "build: HAL_ALIF_DIR unresolved (TBD) — run inside the west workspace or export HAL_ALIF_DIR" >&2
	exit 2
fi

# Resolve the app source dir (accept either an absolute path or one
# relative to the alp-sdk checkout).
if [ -d "$APP" ]; then
	APP_DIR="$APP"
else
	APP_DIR="$ALP_SDK_DIR/$APP"
fi

# Refuse rather than silently drop an overlay/conf fragment (alp-sdk#2094
# review). Zephyr's zephyr_file(CONF_FILES ...) (extensions.cmake) accepts
# EITHER the FULL qualified stem (board + every qualifier segment) OR the
# SHORT stem (board + every qualifier segment except the FIRST -- the SoC
# id); requiring only the full stem would false-refuse a real, Zephyr-valid
# short-form overlay like alp_e1m_aen803_m55_he_rtss_he.overlay. This
# mirrors scripts/check_example_board_overlay_parity.py's
# _qualified_target_to_stems() (dev, #2120) rather than reimplementing it
# ad hoc. .overlay and .conf are matched as INDEPENDENT pools -- Zephyr
# auto-applies each kind separately, so a matching .overlay does not excuse
# a mismatched qualified .conf (or vice versa); only *.overlay/*.conf are
# considered qualified (an unrelated alp_e1m_*-prefixed file, e.g. a
# firmware-update-log *_log_mram.dtsi, is never auto-applied and must not
# trigger a refusal).
#
# Skipped outright when the caller's own extra args already force the
# overlay/conf selection explicitly (-DDTC_OVERLAY_FILE=... or
# -DAPPLICATION_CONFIG_DIR=...) -- configuration_files.cmake skips the
# boards/ auto-apply entirely in that case, so nothing can be silently
# dropped and refusing would only block a deliberate override.
skip_preflight=""
for _a in "$@"; do
	case "$_a" in
	-DDTC_OVERLAY_FILE=* | -DAPPLICATION_CONFIG_DIR=*)
		skip_preflight=1
		;;
	esac
done

if [ -z "$skip_preflight" ] && [ -d "$APP_DIR/boards" ]; then
	# Split $BOARD's qualifier path ("board/qual0/qual1/...") into the FULL
	# stem (every segment) and the SHORT stem (board + every qualifier
	# except qual0, the SoC id) -- bash 3.2 has no readarray/mapfile, so
	# IFS-split via `read -a` into an indexed array instead.
	IFS='/' read -r -a board_parts <<<"$BOARD"
	n_parts=${#board_parts[@]}
	board_full_stem="${board_parts[0]}"
	i=1
	while [ "$i" -lt "$n_parts" ]; do
		board_full_stem="${board_full_stem}_${board_parts[$i]}"
		i=$((i + 1))
	done
	board_short_stem="${board_parts[0]}"
	i=2
	while [ "$i" -lt "$n_parts" ]; do
		board_short_stem="${board_short_stem}_${board_parts[$i]}"
		i=$((i + 1))
	done

	mismatch_report=""
	for ext in overlay conf; do
		qualified_found=""
		found_files=""
		matched=""
		for f in "$APP_DIR"/boards/alp_e1m_*."$ext"; do
			[ -f "$f" ] || continue
			qualified_found=1
			found_files="$found_files $(basename "$f")"
			file_stem="$(basename "$f")"
			file_stem="${file_stem%.*}"
			if [ "$file_stem" = "$board_full_stem" ] || [ "$file_stem" = "$board_short_stem" ]; then
				matched=1
			fi
		done
		if [ -n "$qualified_found" ] && [ -z "$matched" ]; then
			mismatch_report="${mismatch_report}
       .$ext:$found_files"
		fi
	done

	if [ -n "$mismatch_report" ]; then
		echo "build: REFUSING -- $NAME ships qualified boards/ file(s) for a DIFFERENT" >&2
		echo "       board than AEN_BOARD=$BOARD resolves to" >&2
		echo "       (full stem: $board_full_stem, short stem: $board_short_stem)." >&2
		echo "       Mismatched kind(s), found file(s):$mismatch_report" >&2
		echo "       Zephyr auto-applies a boards/ overlay or conf fragment ONLY on an" >&2
		echo "       exact full- or short-stem match, and this build.sh never forces" >&2
		echo "       EXTRA_DTC_OVERLAY_FILE/EXTRA_CONF_FILE for them -- building anyway" >&2
		echo "       would silently drop the mismatched kind (alp-sdk#2094/#2101 class)." >&2
		echo "       Either override AEN_BOARD to the target this app actually ships for," >&2
		echo "       or add boards/${board_full_stem}.<ext> (or the short-stem form" >&2
		echo "       boards/${board_short_stem}.<ext>) to this app." >&2
		exit 3
	fi
fi

cd "$ALP_SDK_DIR"
echo ">>> build $NAME  (board: $BOARD, overlay: auto-applied by FQ board name)" >&2
# The build output is filtered through grep for readability, which means the
# pipeline's status is GREP's, not west's -- and the `|| true` discarded even
# that. Capture west's own status out of PIPESTATUS so a failure is still
# reportable after the filter (alp-sdk#1338).
west build -p always -b "$BOARD" "$APP_DIR" -d "$BD" -- \
	"-DEXTRA_ZEPHYR_MODULES=$ALP_SDK_DIR;$HAL_ALIF_DIR" "$@" 2>&1 |
	grep -iE "error:|warning: .*(undeclared|implicit|conflict)|FATAL|overflow|Memory region|FLASH:|ITCM:|DTCM:|SRAM:|Linking C executable zephyr/zephyr.elf" || true
west_rc=${PIPESTATUS[0]}

# MUST exit non-zero on a failed build (alp-sdk#1338).
#
# This was previously
#     [ -f ... ] && echo "BIN OK: ..." || echo "BUILD FAILED: no zephyr.bin"
# as the script's LAST command, so the `||` branch ran `echo`, `echo`
# succeeded, and the script exited 0 while printing BUILD FAILED. Every
# consumer that gated on `build.sh && <next step>` proceeded on a failed
# build -- RAM-running or flashing a STALE binary from a previous build, or
# reporting a build failure as a run-time "no RESULT" and blaming the app or
# the board for a toolchain error.
#
# `zephyr.bin` is the assertion worth keeping: it is the artefact every
# downstream flow consumes (ram-run.sh loadbin, the MRAM writers), and it is
# absent for every failure mode, not just a configure error.
if [ -f "$BD/zephyr/zephyr.bin" ]; then
	echo "BIN OK: $BD/zephyr/zephyr.bin ($(stat -c%s "$BD/zephyr/zephyr.bin") B)"
else
	echo "BUILD FAILED: no zephyr.bin at $BD/zephyr/zephyr.bin (west exit ${west_rc})" >&2
	exit 1
fi
