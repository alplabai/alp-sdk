#!/usr/bin/env bash
# scripts/bench/aen/build.sh <app-src-dir> [extra -D args...]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh).
# Runs under WSL2 on Windows; the west build itself is cross-platform
# but this wrapper assumes a POSIX shell. See docs/aen-bench-bringup.md
# and scripts/bench/aen/README.md.
#
# Pristine-build an AEN bench app for the E8 M55-HE target.
# Overlays and board-qualified .conf files auto-apply: this builds the
# fully-qualified $AEN_BOARD target (default
# alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he), so Zephyr picks up
# boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay (and the
# matching .conf) plus app.overlay by name automatically -- no explicit
# -DEXTRA_DTC_OVERLAY_FILE force needed (the examples ship fully-qualified
# overlay/.conf names, not the bare board name that would silently drop).
# For a Flow C RAM-run, pass the bench-only ITCM retarget explicitly -- it
# is NOT one of the auto-applied overlays above (it lives outside the app,
# on purpose: the retarget is a bench concern, not something any app's own
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

# Zephyr auto-applies a per-app devicetree overlay, AND a per-app
# board-qualified .conf, ONLY when its filename matches the
# fully-qualified board target with '/' replaced by '_'. This script
# never forces EXTRA_DTC_OVERLAY_FILE/EXTRA_CONF_FILE for either, so an
# app that ships one of these for a DIFFERENT target builds with that
# file silently dropped and says nothing -- the silent-misbuild hazard
# bench-env.sh's AEN_BOARD note describes. That is the failure mode
# #2094 exists to stop: a bench operator gets a binary shaped for the
# wrong module (a missing devicetree edit, or a missing Kconfig default)
# and no indication of it, and the first symptom is a peripheral
# behaving oddly on real silicon.
#
# Refuse both. An app with NO alp_e1m_*-qualified file of a given kind is
# fine -- there is nothing to miss -- so only a non-empty alp_e1m_* set of
# that kind that lacks THIS board's file is an error.
#
# Scoped to alp_e1m_*-prefixed stems ONLY, deliberately not "every file of
# this extension": boards/ can also hold a native_sim_native_64.overlay/
# .conf (a different board family entirely, built by twister, never by
# this script), and an unscoped glob would flag that as a "missing AEN803
# file". That is rare for .overlay but COMMON for .conf -- several
# aen-cc3501e-* bench apps ship ONLY boards/native_sim_native_64.conf, no
# AEN .conf of any kind, and must keep building clean here.
BOARD_STEM="${BOARD//\//_}"

# bench_build_require_board_qualified <ext> <label> — refuse when
# $APP_DIR/boards ships an alp_e1m_*.<ext> file for SOME AEN board target
# but not one named after $BOARD_STEM.
bench_build_require_board_qualified() {
	local ext="$1" label="$2"
	# A SEPARATE `local` statement, deliberately: bash expands every word on
	# a `local a=$1 b=$a` line BEFORE any of that line's assignments take
	# effect, so a same-line reference to $ext here would expand against
	# whatever `ext` held before this call (empty on the first call) --
	# not the "$1" just assigned above.
	local expected="$APP_DIR/boards/$BOARD_STEM.$ext"
	local have=0 f

	for f in "$APP_DIR"/boards/alp_e1m_*."$ext"; do
		[ -e "$f" ] || continue
		have=1
		break
	done
	[ "$have" = 1 ] || return 0
	[ -e "$expected" ] && return 0

	echo "build: $NAME ships AEN board $label files, but none for $BOARD" >&2
	echo "build:   expected: $expected" >&2
	echo "build:   present:" >&2
	for f in "$APP_DIR"/boards/alp_e1m_*."$ext"; do
		[ -e "$f" ] || continue
		echo "build:     $(basename "$f")" >&2
	done
	echo "build: refusing to build with no $label applied (alp-sdk#2094)." >&2
	echo "build: to build anyway, name the board explicitly:" >&2
	echo "build:   AEN_BOARD=<fully-qualified board> $0 $APP" >&2
	exit 2
}

if [ -d "$APP_DIR/boards" ]; then
	bench_build_require_board_qualified overlay "overlay"
	bench_build_require_board_qualified conf ".conf"
fi

cd "$ALP_SDK_DIR"
echo ">>> build $NAME  (overlay: auto-applied by FQ board name)" >&2
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
