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
# (alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he), so Zephyr picks up
# boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay and
# app.overlay by name automatically -- no explicit -DEXTRA_DTC_OVERLAY_FILE
# force needed (the examples ship fully-qualified overlay names, not the
# bare board name that would silently drop). Prints errors + the
# memory-region summary only.
set -e

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

cd "$ALP_SDK_DIR"
echo ">>> build $NAME  (overlay: auto-applied by FQ board name)" >&2
west build -p always -b "$BOARD" "$APP_DIR" -d "$BD" -- \
	"-DEXTRA_ZEPHYR_MODULES=$ALP_SDK_DIR;$HAL_ALIF_DIR" "$@" 2>&1 |
	grep -iE "error:|warning: .*(undeclared|implicit|conflict)|FATAL|overflow|Memory region|FLASH:|ITCM:|DTCM:|SRAM:|Linking C executable zephyr/zephyr.elf" || true
[ -f "$BD/zephyr/zephyr.bin" ] && echo "BIN OK: $BD/zephyr/zephyr.bin ($(stat -c%s "$BD/zephyr/zephyr.bin") B)" || echo "BUILD FAILED: no zephyr.bin"
