# shellcheck shell=bash
# scripts/bench/aen/bench-env.sh
#
# Cross-platform scope: this env layer + the AEN bench helpers that
# source it are Linux-side bench tooling (J-Link CommanderScript +
# the Alif SETOOLS, both Linux binaries on this bench). Windows users
# run them via WSL2; macOS users have the J-Link tools but the Alif
# SETOOLS are Linux-only. There is no native PowerShell equivalent —
# the bench is physically Linux-attached. See docs/aen-bench-bringup.md.
#
# SHARED, SANITIZED env for the AEN801 (Alif Ensemble E8, M55-HE) bench
# flash/RAM-run helpers. SOURCE this (don't execute it):
#
#     source "$(dirname "$0")/bench-env.sh"
#
# Every host-specific value (workspace root, serial device, SETOOLS
# install, J-Link probe) is resolved here from the environment, with
# sensible repo-relative defaults where one exists. Override any of
# them by exporting the variable before invoking a helper, e.g.:
#
#     SE_UART=<your-serial-device> ./flash-run.sh "$BENCH_ROOT/build/aen-gpio-bench"
#
# NOTHING host-specific is hard-coded into the committed scripts — the
# originals lived outside the repo with absolute /home paths; this
# layer is the single place those values come from.

# --------------------------------------------------------------------
# Workspace + Zephyr
# --------------------------------------------------------------------

# BENCH_ROOT — where build outputs live. Defaults to the alp-sdk repo
# root (git toplevel); override to keep build dirs outside the tree.
if [ -z "${BENCH_ROOT:-}" ]; then
	BENCH_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
	# Fall back to two levels up from this file (scripts/bench/aen/..)
	# if we're not inside a git checkout for some reason.
	if [ -z "$BENCH_ROOT" ]; then
		_self="${BASH_SOURCE[0]:-$0}"
		BENCH_ROOT="$(cd "$(dirname "$_self")/../../.." && pwd)"
	fi
fi
export BENCH_ROOT

# ALP_SDK_DIR — the alp-sdk checkout. Same as BENCH_ROOT when you build
# in-tree; kept separate so a future split (build dir != sdk dir) is a
# one-line override.
export ALP_SDK_DIR="${ALP_SDK_DIR:-$BENCH_ROOT}"

# ZEPHYR_BASE — the pinned Zephyr 4.4.0 checkout. west usually exports
# this; resolve it via `west topdir` if unset. Left empty (caller's
# environment is authoritative) when neither is available.
if [ -z "${ZEPHYR_BASE:-}" ]; then
	_topdir="$(west topdir 2>/dev/null || true)"
	[ -n "$_topdir" ] && [ -d "$_topdir/zephyr" ] && ZEPHYR_BASE="$_topdir/zephyr"
fi
export ZEPHYR_BASE

# ZEPHYR_SDK_INSTALL_DIR — root of the Zephyr SDK (the GNU Arm
# arm-zephyr-eabi toolchain lives under
# $ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin). No default — the SDK
# install path is host-specific; export it before sourcing, or let the
# helper fall back to a PATH-resolved arm-zephyr-eabi-* (see
# bench_tool_prefix below).
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-}"

# HAL_ALIF_DIR — the hal_alif Zephyr module, passed to the build as an
# EXTRA_ZEPHYR_MODULE. Resolve it from the west manifest; this is the
# robust, host-agnostic way (the module path is wherever west placed
# it). TBD fallback: if `west list` can't resolve it, the caller MUST
# export HAL_ALIF_DIR — we do NOT invent a path.
if [ -z "${HAL_ALIF_DIR:-}" ]; then
	HAL_ALIF_DIR="$(west list -f '{abspath}' hal_alif 2>/dev/null | head -1 || true)"
fi
export HAL_ALIF_DIR
# If still empty, helpers that need it print: "HAL_ALIF_DIR unresolved
# (TBD) — run inside the west workspace or export HAL_ALIF_DIR".

# --------------------------------------------------------------------
# Board target (the lead part: AEN801 / E8 / M55-HE, RTSS-HE)
# --------------------------------------------------------------------
export AEN_BOARD="${AEN_BOARD:-alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he}"

# --------------------------------------------------------------------
# Serial + SETOOLS (Flow A — production MRAM flash over the SE-UART)
# --------------------------------------------------------------------

# SE_UART — the FT232R SE-UART device SETOOLS' app-write-mram talks to.
# NO default: serial enumeration is host-specific (Linux /dev/ttyUSB*,
# macOS /dev/cu.usbserial-*, Windows COMx). Export it for Flow A.
export SE_UART="${SE_UART:-}"

# SETOOLS_DIR — the Alif Security Toolkit "app-release-exec-linux"
# directory (contains app-gen-toc, app-write-mram, build/). NO default
# and ERROR-IF-UNSET when a Flow A/D helper actually needs it: SETOOLS
# is LICENSE-GATED and is NOT redistributed by alp-sdk (see README.md).
# Obtain it from Alif and export SETOOLS_DIR before running Flow A/D.
export SETOOLS_DIR="${SETOOLS_DIR:-}"

# --------------------------------------------------------------------
# J-Link (reads/RAM-run = generic device; Flow D MRAM flash = part dev)
# --------------------------------------------------------------------

# JLINK_DEVICE_FLASH — the Alif PART-NUMBER device profile. ONLY this
# profile unlocks J-Link's built-in Alif MRAM loader (Flow D). It will
# NOT connect to a live/running secure core (use _READ for that).
export JLINK_DEVICE_FLASH="${JLINK_DEVICE_FLASH:-AE822FA0E5597LS0_M55_HE}"

# JLINK_DEVICE_READ — the GENERIC Cortex-M55 device used for every
# read/attach/RAM-run (it attaches to the live core; the part profile
# cannot re-halt the running SE-booted app).
export JLINK_DEVICE_READ="${JLINK_DEVICE_READ:-Cortex-M55}"

# JLINK_SPEED — SWD clock in kHz.
export JLINK_SPEED="${JLINK_SPEED:-4000}"

# JLINK_SN / JLINK_SERIAL — optional SEGGER probe serial selector. Leave unset
# on a single-probe bench; set it when multiple J-Links are visible on the host.
export JLINK_SN="${JLINK_SN:-${JLINK_SERIAL:-}}"

# --------------------------------------------------------------------
# Tool resolution helpers
# --------------------------------------------------------------------

# bench_tool_prefix — echo the arm-zephyr-eabi toolchain prefix, so a
# caller can run "${PFX}-nm", "${PFX}-readelf", "${PFX}-objdump". Uses
# ZEPHYR_SDK_INSTALL_DIR when set; otherwise falls back to a bare
# "arm-zephyr-eabi" resolved off PATH. Returns non-zero (and prints to
# stderr) if neither resolves.
bench_tool_prefix() {
	local pfx
	if [ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" ] &&
		[ -x "$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm" ]; then
		pfx="$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi"
	elif command -v arm-zephyr-eabi-nm >/dev/null 2>&1; then
		pfx="arm-zephyr-eabi"
	else
		echo "bench-env: cannot resolve arm-zephyr-eabi toolchain — export ZEPHYR_SDK_INSTALL_DIR or put arm-zephyr-eabi-* on PATH" >&2
		return 1
	fi
	echo "$pfx"
}

# bench_jlink_exe — echo the JLink Commander binary name/path. Override
# with JLINK_EXE if your install uses a non-PATH location. The binary
# is "JLinkExe" on Linux/macOS (SEGGER J-Link software pack).
bench_jlink_exe() {
	local exe="${JLINK_EXE:-JLinkExe}"
	if ! command -v "$exe" >/dev/null 2>&1 && [ ! -x "$exe" ]; then
		echo "bench-env: '$exe' not found — install the SEGGER J-Link software or set JLINK_EXE" >&2
		return 1
	fi
	echo "$exe"
}

# bench_require_setools — guard for Flow A/D. Errors (exit 2) if
# SETOOLS_DIR is unset or doesn't look like a SETOOLS install. SETOOLS
# is license-gated and NOT shipped with alp-sdk; this is the single
# enforcement point.
bench_require_setools() {
	if [ -z "${SETOOLS_DIR:-}" ]; then
		echo "bench-env: SETOOLS_DIR is unset. The Alif Security Toolkit is" >&2
		echo "           license-gated and is NOT redistributed by alp-sdk." >&2
		echo "           Obtain it from Alif and: export SETOOLS_DIR=<...>/app-release-exec-linux" >&2
		return 2
	fi
	if [ ! -x "$SETOOLS_DIR/app-gen-toc" ]; then
		echo "bench-env: '$SETOOLS_DIR' does not look like a SETOOLS app-release-exec-linux dir (no app-gen-toc)" >&2
		return 2
	fi
	return 0
}
