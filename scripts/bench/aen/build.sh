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
# matching .conf) by name automatically -- no explicit
# -DEXTRA_DTC_OVERLAY_FILE force needed (the examples ship fully-qualified
# overlay/.conf names, not the bare board name that would silently drop).
# (Zephyr only falls back to a bare app.overlay when no boards/ (or
# socs/) overlay matched -- configuration_files.cmake:76-84, v4.4.1 --
# so an app shipping BOTH a board-qualified overlay and an app.overlay
# gets only the board overlay; the fallback is exactly how app.overlay
# gets applied for an app that ships no boards/ overlay at all, e.g.
# aen-uart-ns16550-loopback, aen-spi-regcheck, aen-npu-ethosu-regcheck,
# aen-npu-ethosu55-regcheck, aen-can-regcheck and aen-dualcore-probe.)
# For a Flow C RAM-run, pass
# the bench-only ITCM retarget explicitly -- it is NOT one of the
# auto-applied overlays above (it lives outside the app, on purpose: the
# retarget is a bench concern, not something any app's own prj.conf/overlay
# should carry) -- both halves together, e.g.:
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
# board-qualified .conf, when its filename matches a board-qualifier stem
# Zephyr itself accepts -- and it accepts TWO: the FULL stem (board +
# every qualifier segment, '/' -> '_') and a SHORT stem that drops the
# FIRST qualifier segment (the SoC id). The short form is legal only for a
# single-SoC board (`BOARD_${BOARD}_SINGLE_SOC`,
# zephyr/cmake/modules/boards.cmake:263-268, v4.4.1) -- both AEN803 HE and
# HP declare exactly one SoC, so both stems are live targets here.
# Zephyr computes the pair identically for .overlay and .conf:
# zephyr_build_string() builds the full stem
# (cmake/modules/extensions.cmake:1717-1720) and the short one via
#   string(REGEX REPLACE "^.[^/]*(.*)" "\\1" shortened_qualifiers "${BOARD_QUALIFIERS}")
# (extensions.cmake:1736), and zephyr_file(CONF_FILES ...) tries both
# candidates for EITHER extension (extensions.cmake:2892-3013). Python
# sibling of this same derivation, used elsewhere in this repo:
# check_example_board_overlay_parity.py's _qualified_target_to_stems().
#
# This script never forces EXTRA_DTC_OVERLAY_FILE/EXTRA_CONF_FILE for
# either, so an app that ships one of these for a DIFFERENT target builds
# with that file silently dropped and says nothing -- the silent-misbuild
# hazard bench-env.sh's AEN_BOARD note describes. That is the failure mode
# #2094 exists to stop: a bench operator gets a binary shaped for the
# wrong module (a missing devicetree edit, or a missing Kconfig default)
# and no indication of it, and the first symptom is a peripheral behaving
# oddly on real silicon.
#
# Refuse both kinds. An app with NO board-qualified file of a given kind is
# fine -- there is nothing to miss -- so only a non-empty board-qualified
# set of that kind that matches NEITHER of this board's two accepted
# stems is an error.
#
# Scoped to alp_e1m_*_rtss_h[ep]-shaped stems ONLY, deliberately not
# "every alp_e1m_* file of this extension" and not "every file of this
# extension": boards/ can also hold a native_sim_native_64.overlay/.conf
# (a different board family entirely, built by twister, never by this
# script), and #2094's original unscoped overlay loop would have flagged
# that as a "missing AEN803 file". On this tree that shape is more common
# for .overlay -- 34 examples ship boards/ overlays with none
# alp_e1m_*-qualified, e.g. examples/peripheral-io/gpio-button-led ships
# only boards/native_sim_native_64.overlay -- than for .conf (26
# examples, e.g. several aen-cc3501e-* bench apps ship ONLY
# boards/native_sim_native_64.conf, no AEN .conf of any kind); both
# extensions must keep building clean here regardless.
#
# The narrower "..._rtss_h[ep]" suffix (every real AEN board-qualified
# stem this script ever resolves to ends in "_rtss_he" or "_rtss_hp" --
# see $BOARD_STEM/$BOARD_SHORT_STEM below) also excludes an
# alp_e1m_*-prefixed file that is NOT board-qualified at all:
# examples/connectivity/firmware-update-log ships
# boards/alp_e1m_aen801_m55_he_firewall_probe.conf and
# ..._firewall_proven.conf, passed explicitly via that app's own
# CMakeLists.txt EXTRA_CONF_FILE, never picked up by Zephyr's board-name
# auto-apply rule. A plain alp_e1m_* glob would count those as "have a
# board-qualified .conf" and could one day refuse an app that ships ONLY
# such fragments for a file that was never going to be auto-applied in
# the first place.
#
# That "_rtss_h[ep]" scoping only proves an app ships SOME real
# board-qualified file -- not that it ships one THIS board would ever
# apply. A file misnamed for THIS board specifically slips past that glob
# entirely: boards/<board>.$ext (bare, no qualifier -- e.g.
# alp_e1m_aen803_m55_he.overlay) or boards/<board>_<soc>.$ext (board+SoC,
# missing the trailing RTSS qualifier -- e.g.
# alp_e1m_aen803_m55_he_ae822fa0e5597ls0.overlay). Zephyr auto-applies
# NEITHER (both accepted stems require the RTSS qualifier), so these are
# silent-misbuild hazards exactly like the mismatched-stem case above, just
# invisible to the generic glob. $BOARD_BARE/$BOARD_BOARD_SOC below name
# them; they cannot collide with the `_firewall_*` EXTRA_CONF_FILE
# fragments above (those always carry a `_firewall_probe`/`_firewall_proven`
# suffix past the full stem, never a bare board or board+SoC filename).

# BOARD_STEM (full) and BOARD_SHORT_STEM (drops the first '/'-qualifier,
# i.e. the SoC id) -- the two filenames Zephyr itself will auto-apply for
# $BOARD (see the citation above). Splitting on '/' via an array, not a
# string op, so a $BOARD with one qualifier segment or none is handled the
# same way the Python sibling handles it (short == the bare board name, or
# short == the full stem) rather than as a special case.
#
# Refuse a malformed $BOARD up front: a leading '/', a trailing '/', or an
# empty qualifier segment ('//'). Zephyr accepts '//' as shorthand for the
# omitted SoC of a single-SoC board (e.g. alp_e1m_aen803_m55_he//rtss_he,
# boards.cmake:271-272, v4.4.1), but resolves it during its own board
# lookup, which this preflight cannot reproduce -- the stems derived below
# would be wrong. A trailing '/' is subtler: `IFS='/' read -a` silently
# drops the empty trailing field, so the stems would look well-formed and
# the malformed target would reach `west build -b` unchanged. Ask for the
# fully-qualified board instead of guessing.
case "$BOARD" in
/*|*/|*//*)
	echo "build: BOARD '$BOARD' has a leading '/', a trailing '/', or an" >&2
	echo "build:   empty qualifier segment ('//')." >&2
	echo "build:   Zephyr allows omitting the SoC for a single-SoC board via '//', but" >&2
	echo "build:   this preflight does not resolve which SoC that means, and a leading" >&2
	echo "build:   or trailing '/' is never valid. Name the board fully qualified" >&2
	echo "build:   instead, e.g.:" >&2
	echo "build:   AEN_BOARD=alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he $0 $APP" >&2
	exit 2
	;;
esac
IFS='/' read -r -a _board_parts <<<"$BOARD"
# Joined from the array so BOARD_STEM, BOARD_SHORT_STEM and BOARD_BARE all
# derive from the same parse of $BOARD.
BOARD_STEM="${_board_parts[0]}"
for _q in "${_board_parts[@]:1}"; do
	BOARD_STEM="${BOARD_STEM}_${_q}"
done
if [ "${#_board_parts[@]}" -le 2 ]; then
	# No qualifier segment, or exactly one -- dropping the (only)
	# qualifier leaves just the board name.
	BOARD_SHORT_STEM="${_board_parts[0]}"
else
	BOARD_SHORT_STEM="${_board_parts[0]}"
	for _q in "${_board_parts[@]:2}"; do
		BOARD_SHORT_STEM="${BOARD_SHORT_STEM}_${_q}"
	done
fi
# BOARD_BARE (just the board name) and BOARD_BOARD_SOC (board+SoC,
# dropping only the trailing RTSS qualifier) -- the two near-miss names a
# typo can produce for THIS board specifically (see the comment above).
# BOARD_BOARD_SOC only exists as a name DISTINCT from BOARD_STEM/
# BOARD_SHORT_STEM when there are 3+ segments (board/soc/rtss); with
# fewer, "board+first qualifier" already IS one of the two accepted
# stems, not a near miss, so it is left empty and the caller skips it.
BOARD_BARE="${_board_parts[0]}"
if [ "${#_board_parts[@]}" -ge 3 ]; then
	BOARD_BOARD_SOC="${_board_parts[0]}_${_board_parts[1]}"
else
	BOARD_BOARD_SOC=""
fi
unset _board_parts _q

# bench_build_require_board_qualified <ext> <label> — refuse when
# $APP_DIR/boards ships an alp_e1m_*_rtss_h[ep].<ext> file (board-
# qualified for SOME AEN board target), or a near-miss name for THIS
# board specifically (bare board, or board+SoC), but neither $BOARD_STEM
# nor $BOARD_SHORT_STEM is present. Also refuses when BOTH $BOARD_STEM and
# $BOARD_SHORT_STEM are present at once -- Zephyr's own zephyr_file(
# CONF_FILES) FATAL_ERRORs on that pair ("Conflicting file names
# discovered", extensions.cmake:2950 overlay / :2997 conf, v4.4.1) with a
# message this script's own output filter (see the grep below) would
# otherwise drop.
bench_build_require_board_qualified() {
	local ext="$1" label="$2"
	# A SEPARATE `local` statement, deliberately: bash expands every word on
	# a `local a=$1 b=$a` line BEFORE any of that line's assignments take
	# effect, so a same-line reference to $ext here would expand against
	# whatever `ext` held before this call (empty on the first call) --
	# not the "$1" just assigned above.
	local expected_full="$APP_DIR/boards/$BOARD_STEM.$ext"
	local expected_short="$APP_DIR/boards/$BOARD_SHORT_STEM.$ext"
	local near_bare="$APP_DIR/boards/$BOARD_BARE.$ext"
	local near_board_soc=""
	[ -n "$BOARD_BOARD_SOC" ] && near_board_soc="$APP_DIR/boards/$BOARD_BOARD_SOC.$ext"
	local have=0 f

	# The inequality guard matters for a zero-qualifier $BOARD (no '/' at
	# all): BOARD_STEM and BOARD_SHORT_STEM are then the same string, so
	# $expected_full and $expected_short are the same path, and without
	# the guard a single real file would read as "both stems present"
	# and print that one path twice.
	if [ "$expected_full" != "$expected_short" ] &&
		[ -e "$expected_full" ] && [ -e "$expected_short" ]; then
		echo "build: $NAME ships BOTH the full and short board-qualified $label" >&2
		echo "build: stems for $BOARD -- Zephyr rejects the pair:" >&2
		echo "build:   $expected_full" >&2
		echo "build:   $expected_short" >&2
		echo "build: keep the full stem ($(basename "$expected_full")); it is the one" >&2
		echo "build: Zephyr's own \"Conflicting file names discovered\" error recommends." >&2
		exit 2
	fi

	for f in "$APP_DIR"/boards/alp_e1m_*_rtss_h[ep]."$ext"; do
		[ -e "$f" ] || continue
		have=1
		break
	done
	# This board's OWN near-miss names also count as "ships a file of this
	# kind" -- Zephyr applies neither (see the comment above BOARD_BARE),
	# so leaving them out of $have would let the guard pass an app that
	# silently drops its own overlay/.conf. A <=2-segment $BOARD makes
	# $BOARD_BARE == $BOARD_SHORT_STEM, so $near_bare can coincide with
	# $expected_short here -- that is not a near miss, it is the accepted
	# file, but it needs no special-casing: the early-return below already
	# exits on $expected_short before the "ships AEN board" error is ever
	# reached, so setting $have=1 for it too is harmless.
	if [ -e "$near_bare" ]; then
		have=1
	fi
	if [ -n "$near_board_soc" ] && [ -e "$near_board_soc" ]; then
		have=1
	fi
	[ "$have" = 1 ] || return 0
	[ -e "$expected_full" ] && return 0
	[ -e "$expected_short" ] && return 0

	echo "build: $NAME ships AEN board $label files, but none for $BOARD" >&2
	echo "build:   expected either: $expected_full" >&2
	echo "build:              or:   $expected_short" >&2
	echo "build:   present:" >&2
	for f in "$APP_DIR"/boards/alp_e1m_*_rtss_h[ep]."$ext"; do
		[ -e "$f" ] || continue
		echo "build:     $(basename "$f")" >&2
	done
	[ -e "$near_bare" ] && echo "build:     $(basename "$near_bare")" >&2
	[ -n "$near_board_soc" ] && [ -e "$near_board_soc" ] &&
		echo "build:     $(basename "$near_board_soc")" >&2
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
echo ">>> build $NAME  (overlay/.conf: auto-applied by full or short board stem)" >&2
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
