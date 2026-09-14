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
# Zephyr picks up boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay
# and app.overlay by name automatically -- no explicit -DEXTRA_DTC_OVERLAY_FILE
# force needed (the examples ship fully-qualified overlay names, not the
# bare board name that would silently drop). Most AEN examples have not yet
# grown an AEN803-qualified overlay (alp-sdk#2101); this script REFUSES to
# build an app whose boards/ ships a qualified overlay for a different board
# than $AEN_BOARD resolves to, rather than silently dropping it (alp-sdk#2094)
# -- override AEN_BOARD for such an app, e.g.:
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

# Refuse rather than silently drop an overlay (alp-sdk#2094). Zephyr
# auto-applies a boards/ overlay/conf only on an EXACT match of the
# fully-qualified board name (qualifier path with every "/" -> "_"); if
# $APP_DIR ships >=1 alp_e1m_*-qualified boards/ file at all but NONE of
# them is stem-equal to $BOARD's stem, building it anyway would produce a
# silently wrong (e.g. AEN801-shaped) image on the target the operator
# actually asked for -- the exact #2101 hazard AEN_BOARD's default repoint
# to AEN803 (#2094) introduced across every example that has not yet grown
# an AEN803 overlay.
board_stem="$(printf '%s' "$BOARD" | tr '/' '_')"
if [ -d "$APP_DIR/boards" ]; then
	qualified_found=""
	matched=""
	for f in "$APP_DIR"/boards/alp_e1m_*; do
		[ -f "$f" ] || continue
		qualified_found=1
		stem="$(basename "$f")"
		stem="${stem%.*}"
		if [ "$stem" = "$board_stem" ]; then
			matched=1
			break
		fi
	done
	if [ -n "$qualified_found" ] && [ -z "$matched" ]; then
		echo "build: REFUSING -- $NAME ships a qualified boards/ overlay for a DIFFERENT" >&2
		echo "       board than AEN_BOARD=$BOARD resolves to (stem: $board_stem)." >&2
		echo "       $APP_DIR/boards/ contains:" >&2
		for f in "$APP_DIR"/boards/alp_e1m_*; do
			[ -f "$f" ] && echo "         $(basename "$f")" >&2
		done
		echo "       Zephyr auto-applies an overlay ONLY on an exact board-name match, and" >&2
		echo "       this build.sh never forces EXTRA_DTC_OVERLAY_FILE -- building anyway" >&2
		echo "       would silently drop the overlay (alp-sdk#2094/#2101 class)." >&2
		echo "       Either override AEN_BOARD to the target this app actually ships an" >&2
		echo "       overlay for, or add boards/${board_stem}.overlay to this app." >&2
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
