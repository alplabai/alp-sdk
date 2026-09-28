#!/usr/bin/env bash
# scripts/bench/aen/erase-storage.sh [--dry-run] [--check-only] [--sku <SKU>]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh; drives
# JLinkExe, a Linux binary on this bench). Run it under WSL2 on Windows; macOS
# has the J-Link tools but is not the bench host. See docs/aen-provisioning.md.
#
# PROVISIONING STEP (alp-sdk#1430) -- erase the CUSTOMER STORAGE WINDOW
# before a SoM ships, so the module does not leave manufacturing carrying a
# previous application's image in the window the customer's first NVS write
# lands in. Defaults to the E1M-AEN801 preset (the original, bench-verified
# target); pass `--sku <SKU>` or export `ALP_AEN_SKU=<SKU>` for another AEN
# SKU (e.g. E1M-AEN803, whose `storage`/`atoc` memory_map rows are IDENTICAL
# to E1M-AEN801's -- alp-sdk#2233 review blocker 2: this script used to
# hard-code E1M-AEN801.yaml regardless of which board was actually attached).
#
# WHY: alp-sdk#1334 measured, on E8 silicon, ~110 KiB of a stale
# previously-flashed Zephyr application image sitting in what was then the
# `storage` partition. It is not live data -- but a customer who dumps the part
# sees another application's shell strings, and a first NVS write silently
# destroys bytes that look meaningful. #1334 closed on the measurement; #1430
# is the standing "erase before ship" step it left behind.
#
# [BENCH-VERIFIED 2026-08-30] Run once against a real module (off-labgrid
# E1M-AEN801, `AE822FA0E5597LS0`, J-Link `000821005680`): the window
# 0x80560000 .. 0x80578000 verified all-0x00, and a cold power-cycle showed
# `u VB` on the `ALP-HE` boot row, so the ATOC band was undisturbed by the
# erase. Transcript in docs/aen-provisioning.md section 7 (that transcript
# predates the alp-sdk#2233 fix below and still shows the old `verifybin`
# line). One bench run is not a standing guarantee for a different module --
# the DPIDR gate, the ATOC-trailer overlap check, and the fresh-session
# read-back proof below still run, and must still pass, on every unit.
#
# ---------------------------------------------------------------------------
# HAZARDS -- read before running
# ---------------------------------------------------------------------------
#
# 1. DESTRUCTIVE AND IRREVERSIBLE. This writes the erased pattern over the
#    whole customer storage window. There is no backup and no undo. Anything a
#    customer or a test app has already stored there is gone.
#
# 2. THE ERASED VALUE ON THIS MRAM IS 0x00, NOT 0xFF (#1430, measured from the
#    running application's own flash parameters: `write_block_size=16
#    erase_value=0x00`). That is why the pattern written below comes from
#    /dev/zero, and why any "is this window erased?" check must compare against
#    0x00. Writing 0xFF would leave the window looking programmed, not erased.
#
# 3. A J-LINK `erase` DOES NOT CLEAR MRAM ON THIS PART -- see the GOTCHA in
#    scripts/bench/aen/flash-jlink-mramxip.sh. So the erase is performed as a
#    `loadbin` of a zero-filled file through the part-number device profile
#    ($JLINK_DEVICE_FLASH), which is the only profile that unlocks J-Link's
#    built-in Alif MRAM loader (same mechanism as Flow D / flash-jlink.sh).
#    The SETOOLS/SE-UART alternative, if you would rather stay on Flow A, is
#    `app-write-mram -c $SE_UART -e "<base> <size>"` (that invocation shape is
#    recorded in flash-jlink-mramxip.sh for the slot0 window); this script does
#    not use it, because the SWD path gets the DPIDR board-identity gate below.
#
# 4. THE BAND IMMEDIATELY ABOVE THE WINDOW IS SE-OWNED. `atoc` starts where
#    `storage` ends (metadata/e1m_modules/<SKU>.yaml). SETOOLS top-anchors
#    the signed ATOC there and grows it downward, so an overshoot of even one
#    byte can land in the live ATOC -- the board then boots to `No ATOC` and
#    needs re-provisioning over the SE-UART (docs/aen-provisioning.md section 4).
#    The window is therefore DERIVED from the preset and asserted adjacent to
#    `atoc` below; nothing here is a hardcoded address.
#
#    THE ALLOCATED `atoc` REGION IS NOT WHERE THE LIVE PACKAGE ACTUALLY
#    STARTS (alp-sdk#2233 review blocker 2). SETOOLS top-anchors the signed
#    ATOC at the region's own END and grows it DOWNWARD by however much it
#    actually needs -- on a real E1M-AEN803 bench board, serial
#    2026W36-0001 (2026-09-19), the resident package was measured starting
#    at `0x8056A3C0`, well INSIDE the nominal `storage` window this script
#    erases. A metadata-window-only erase would have zeroed a live,
#    no-SE-UART-recoverable boot table. Before any write, this script now
#    reads the trailer SETOOLS leaves at the top of the `atoc` region
#    (scripts/bench/aen/atoc_trailer.py -- BENCH-MEASURED, ONE BOARD, not a
#    documented SETOOLS layout) and refuses the erase outright if the
#    resolved package start falls inside the window it is about to write.
#
# 5. WRONG-BOARD RISK. This bench has three J-Links and two share OEM serial
#    603000869 (see bench-env.sh). Zeroing an MRAM range on the wrong target
#    corrupts a different, live board. The DPIDR gate is a hard abort, not a
#    warning.
#
# Address-range note: `loadbin` into MRAM is bench-established at 0x80010000
# (docs/aen-provisioning.md section 0.5 Option B) and at the ATOC package
# address near the top of the window (docs/aen-bench-bringup.md, the "Burning:"
# address) -- i.e. below and above this range -- and the storage window
# itself was written this way and byte-verified on 2026-08-30 (see the
# [BENCH-VERIFIED] note above).
#
# --check-only (alp-sdk#2233 review round 4): a READ-ONLY mode -- runs the
# DPIDR preflight and the whole-atoc-region trailer read exactly as the real
# path does (so the overlap decision itself is exercised on real silicon,
# not just the arithmetic around it), then exits BEFORE the pre-write
# backup, bench_flowd_prepare_write, or any write-session CommandFile is
# ever built. See "CHECK-ONLY" below for its own exit codes. Mutually
# exclusive with --dry-run/FLOWD_DRY_RUN, which is the OPPOSITE promise (no
# probe opened at all) -- combining them is a usage error (exit 1), not a
# silently-resolved ambiguity.
#
# Exit codes (aligned with the sibling Flow D helpers):
#   0  window written and byte-verified (fresh-session read-back proof) as
#      erased -- OR, under --check-only, the read-only trailer check
#      completed and the erase WOULD proceed (nothing written in that mode)
#   1  usage error -- an unrecognised argument, --sku with no value, or
#      --check-only combined with --dry-run/FLOWD_DRY_RUN
#      (nothing written, no probe opened)
#   2  the part-number device profile could not connect (nothing written)
#   3  the read-back proof failed -- do NOT treat as erased
#   4  DPIDR gate: this is not the AEN E8 (nothing written)
#   5  the storage window could not be derived from the preset, or it is not
#      adjacent to `atoc` (nothing written); or the sector-pad prepare step
#      (alp-sdk#2233 -- plan/pre-read/build) itself refused or failed
#   6  the ATOC trailer could not be read/parsed, or is internally
#      inconsistent -- the resident package's location cannot be determined
#      safely, so the erase is refused outright (nothing written); under
#      --check-only this means "undeterminable", not "refused a real erase"
#   7  the resolved resident ATOC package overlaps the erase window --
#      refused outright (nothing written); under --check-only this means
#      "the real erase WOULD be refused for this reason", not that this run
#      refused one
#  11  RACE DETECTED between the pre-read and the write session's own
#      pre-load savebin (see bench-env.sh) -- the board HAS already been
#      written; restore from the printed paths before trusting it
#  13  bench_jlink_run()'s FLOWD_DRY_RUN backstop refused the write session
#      (nothing written) -- should never be reachable in practice, since
#      FLOWD_DRY_RUN is now treated exactly like --dry-run and exits before
#      any write session is even built; kept as defense in depth, same as
#      every other Flow D writer
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

DRY_RUN=0
CHECK_ONLY=0
SKU="${ALP_AEN_SKU:-E1M-AEN801}"
# Whole-argv scan (same shape flash-jlink.sh's flag parser uses, see its own
# PARSER SHAPE note) -- --dry-run and --sku may land in either order.
#
# REJECT UNKNOWN ARGS AND A VALUE-LESS --sku (alp-sdk#2233 review round 3,
# finding 11) -- an earlier version silently dropped anything it didn't
# recognise into an unused POSITIONAL array: a `--dryrun` typo (missing the
# hyphen) ran a REAL, destructive erase instead of refusing, and a trailing
# bare `--sku` with no value silently kept targeting the E1M-AEN801 default
# instead of erroring. This script takes no operands, so ANY unrecognised
# argument is a usage mistake, not something to ignore.
_next_is_sku=0
for arg in "$@"; do
	if [ "$_next_is_sku" = 1 ]; then
		SKU="$arg"
		_next_is_sku=0
		continue
	fi
	case "$arg" in
	--dry-run) DRY_RUN=1 ;;
	--check-only) CHECK_ONLY=1 ;;
	--sku) _next_is_sku=1 ;;
	--sku=*) SKU="${arg#--sku=}" ;;
	*)
		echo "!! ABORT: unknown argument '$arg' -- usage: erase-storage.sh [--dry-run] [--check-only] [--sku <SKU>]" >&2
		exit 1
		;;
	esac
done
if [ "$_next_is_sku" = 1 ]; then
	echo "!! ABORT: --sku requires a value -- usage: erase-storage.sh [--dry-run] [--check-only] [--sku <SKU>]" >&2
	exit 1
fi
# alp-sdk#2233 review round 3, finding 2: FLOWD_DRY_RUN must be treated
# EXACTLY like this script's own --dry-run -- an earlier version let
# FLOWD_DRY_RUN reach the DPIDR preflight, the ATOC-trailer read, and the
# pre-write backup copy for real, backing up SYNTHETIC all-zero sectors
# (bench_flowd_read_sectors' own FLOWD_DRY_RUN synthesis) into
# $BENCH_ROOT/flowd-backup/... as if they were a genuine pre-write capture,
# and relied on bench_jlink_run()'s backstop (return 13) to stop the actual
# write -- a backstop, not a substitute for exiting before ANY of that runs.
# Aliasing the two flags here reuses the SAME --dry-run code path below
# (DPIDR skipped, ATOC-trailer read skipped, backup skipped, prints the
# CommandFile and exits 0) rather than adding a second, parallel dry-run
# branch.
if [ -n "${FLOWD_DRY_RUN:-}" ]; then
	DRY_RUN=1
fi

# alp-sdk#2233 review round 4: --check-only is the OPPOSITE promise from
# --dry-run/FLOWD_DRY_RUN -- it deliberately DOES open a probe (read-only)
# to run the DPIDR preflight and the whole-atoc-region trailer read for
# real, so the overlap decision itself is exercised on real silicon rather
# than only its arithmetic. Combining the two is a usage mistake, not an
# ambiguity to silently resolve (whichever alias won would surprise the
# other). Checked AFTER the FLOWD_DRY_RUN-aliases-DRY_RUN block above, so a
# combination via FLOWD_DRY_RUN is caught exactly like one via --dry-run.
if [ "$CHECK_ONLY" = 1 ] && [ "$DRY_RUN" = 1 ]; then
	echo "!! ABORT: --check-only and --dry-run/FLOWD_DRY_RUN are mutually exclusive --" >&2
	echo "   --check-only needs a REAL read of the ATOC trailer; --dry-run/FLOWD_DRY_RUN" >&2
	echo "   opens no probe at all." >&2
	exit 1
fi
if [ "$CHECK_ONLY" = 1 ]; then
	# DEFENSE IN DEPTH, same shape as every other Flow D writer's own
	# FLOWD_DRY_RUN check: bench_jlink_run()'s backstop (bench-env.sh)
	# independently refuses (rc=13) any CommandFile containing a
	# loadbin/erase line while this is set. --check-only's PRIMARY
	# guarantee is structural -- it exits (see below, right after the
	# ATOC-trailer check) before any write-session CommandFile is ever
	# built, so this backstop should be unreachable; it exists for the
	# case a future edit moves that exit and breaks the structural
	# guarantee. Exported HERE, deliberately AFTER the FLOWD_DRY_RUN-
	# aliases-DRY_RUN check above: exporting it earlier would ALSO make
	# that check set DRY_RUN=1 and skip the DPIDR preflight and the
	# trailer read this whole mode exists to run for real. Neither the
	# preflight nor the trailer-read CommandFile ever contains a
	# loadbin/erase line, so this export does not affect either of them.
	export FLOWD_DRY_RUN=1
fi

# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns. DRY_RUN never
# reaches a call site below, so this never opens a probe in that mode.
JLINK_ARGS=(bench_jlink_run)

# 1. DERIVE the window from the SoM preset -- single source of truth, so a
#    future layout move cannot leave a stale address baked in here. Then assert
#    it ends exactly where the SE-owned `atoc` band begins (hazard 4).
#    SKU is NOT hard-coded (alp-sdk#2233 review blocker 2) -- `--sku`/
#    `ALP_AEN_SKU` picks which preset this run targets; the default
#    (E1M-AEN801) is unchanged from before this fix.
PRESET="$ALP_SDK_DIR/metadata/e1m_modules/$SKU.yaml"
if [ ! -r "$PRESET" ]; then
	echo "!! ABORT: cannot read $PRESET -- run this from inside the alp-sdk checkout," >&2
	echo "   and confirm --sku/ALP_AEN_SKU ($SKU) names a real metadata/e1m_modules preset." >&2
	exit 5
fi

# <region-name> <field> -> the field's value off that memory_map row.
_region_field() {
	sed -n "s/.*name:[[:space:]]*$1,.*[[:space:]]$2:[[:space:]]*\([0-9a-fA-Fx]*\).*/\1/p" "$PRESET" | head -1
}
BASE=$(_region_field storage base)
KIB=$(_region_field storage size_kib)
ATOC_BASE=$(_region_field atoc base)
ATOC_KIB=$(_region_field atoc size_kib)
if [ -z "$BASE" ] || [ -z "$KIB" ] || [ -z "$ATOC_BASE" ] || [ -z "$ATOC_KIB" ]; then
	echo "!! ABORT: could not parse the storage/atoc memory_map regions out of" >&2
	echo "   $PRESET -- refusing to guess an MRAM address. Fix the parse, or the" >&2
	echo "   preset moved (storage=$BASE size_kib=$KIB atoc=$ATOC_BASE atoc_size_kib=$ATOC_KIB)." >&2
	exit 5
fi
SIZE=$((KIB * 1024))
END=$((BASE + SIZE))
ATOC_END=$((ATOC_BASE + ATOC_KIB * 1024))
if [ "$END" -ne $((ATOC_BASE)) ]; then
	printf '!! ABORT: storage window %s + %s KiB ends at 0x%X, but atoc starts at %s.\n' \
		"$BASE" "$KIB" "$END" "$ATOC_BASE" >&2
	echo "   The layout moved. Writing this range could land in the SE-owned ATOC" >&2
	echo "   band and leave the board at 'No ATOC'. Re-read the preset first." >&2
	exit 5
fi
printf '>>> SKU: %s (%s)\n' "$SKU" "$PRESET" >&2
printf '>>> customer storage window: %s .. 0x%X (%s KiB, exclusive of atoc at %s .. 0x%X)\n' \
	"$BASE" "$END" "$KIB" "$ATOC_BASE" "$ATOC_END" >&2

# 2. Build the erased pattern. 0x00 IS the erased value on this MRAM (hazard 2),
#    so the file is literally $SIZE zero bytes -- and it is the blob the
#    sector-pad machinery below both writes and later read-back-proves,
#    which makes "erased" a byte-compare rather than a claim.
ZEROS="${TMPDIR:-/tmp}/aen-storage-erased.bin"
head -c "$SIZE" /dev/zero > "$ZEROS"

# The path handed to the J-Link CommanderScript has to be one the J-Link BINARY
# can open, which is not always the one this shell sees.  On a Windows bench
# host (Git Bash / MSYS driving JLink.exe, a native Windows binary) "/tmp/..."
# is meaningless to the callee and the run dies with
#     Failed to open file.
#     ERROR: Could not open file.
# -- the same trap ti/regen_flashset.sh hit.  The verify gate below catches it
# and correctly reports NOT erased, but the erase never happens.  Convert when a
# converter exists; on Linux cygpath is absent and $ZEROS is already right.
ZEROS_FOR_JLINK="$ZEROS"
if command -v cygpath >/dev/null 2>&1; then
	ZEROS_FOR_JLINK="$(cygpath -w "$ZEROS")"
fi

printf '    erased pattern: %s (%s B of 0x00 -- NOT 0xFF)\n' \
	"$ZEROS_FOR_JLINK" "$(wc -c < "$ZEROS" | tr -d ' ')" >&2

# 3. SAFETY GATE -- prove the AEN E8 answered BEFORE any write (hazard 5).
#    Read-only connect with the generic device, same gate as flash-jlink.sh.
#
#    This runs BEFORE any other probe touch (including the sector pre-read
#    below) and before the erase CommanderScript is written: a failed gate
#    then leaves no destructive script staged in /tmp for someone to run by
#    hand, and nothing has read from a possibly-wrong board either.
#
#    DRY_RUN never reaches here, so it still opens no probe.
if [ "$DRY_RUN" != 1 ]; then
	cat > "${TMPDIR:-/tmp}/aen-erase-preflight.jlink" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/aen-erase-preflight.jlink" \
		> "${TMPDIR:-/tmp}/aen-erase-preflight.out" 2>&1 || true
	bench_jlink_assert_aen_dpidr "${TMPDIR:-/tmp}/aen-erase-preflight.out" "storage-erase preflight" || exit 4
	echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2
fi

# ATOC TRAILER CHECK (alp-sdk#2233 review blocker 2) -- BEFORE any write:
# determine where the resident ATOC package actually STARTS (not just where
# its allocated region begins) and refuse outright if this erase would touch
# it. scripts/bench/aen/atoc_trailer.py documents the bench-measured, ONE
# real E1M-AEN803 board (serial 2026W36-0001) trailer layout this reads --
# see that file's own header for exactly what is and is not verified spec.
#
# Read-only, in the same read-only pre-read spirit as the sector-pad plan
# below -- this only ever SAVEBINs, never LOADBINs. DRY_RUN never reaches
# here (no probe opened at all in that mode, hazard-driven -- alp-sdk#2233
# review round 3, finding 2: FLOWD_DRY_RUN now aliases DRY_RUN=1 too, not
# just this script's own --dry-run), so the overlap check below is simply
# SKIPPED under DRY_RUN -- it inherently needs a real read to answer; that
# is a known, printed limitation, not a silent gap.
#
# READS THE WHOLE ATOC REGION, not just its trailer-bearing top sector
# (alp-sdk#2233 review round 3, finding 5): a blank TRAILER does not by
# itself prove there is no resident package (see atoc_trailer.py's own
# blank-trailer handling) -- it also needs to search for a live OEMTOC01
# signature elsewhere in the region before trusting "blank". `atoc`'s own
# size (32 KiB, two sectors on every AEN E8 SKU today) makes reading the
# whole thing here just as cheap as reading one sector, and removes the
# need for a second, separate read.
if [ "$DRY_RUN" != 1 ]; then
	ATOC_REGION_SIZE=$((ATOC_KIB * 1024))
	TRAILER_SECTOR_BASE="$ATOC_BASE"
	TRAILER_SECTOR_FILE="$(mktemp "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.bin.XXXXXX")" || exit 6
	cat > "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.jlink" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
savebin $(bench_flowd_jlink_path "$TRAILER_SECTOR_FILE") $(printf '0x%X' "$((TRAILER_SECTOR_BASE))") $(printf '0x%X' "$ATOC_REGION_SIZE")
exit
EOF
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.jlink" \
		> "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.out" 2>&1 || true
	bench_jlink_assert_connected "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.out" "ATOC trailer read" || exit 6
	# alp-sdk#2233 review round 5: require the bench-measured savebin SUCCESS
	# line (see bench_flowd_read_sectors' identical check in bench-env.sh,
	# same bench measurement) for this session's ONE savebin -- a connected
	# session that never actually reported reading the region must not be
	# trusted just because a right-sized (mktemp'd) file exists.
	if ! grep -qE 'Reading [0-9]+ bytes from addr 0x[0-9A-Fa-f]+ into file.*O\.K\.' "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.out"; then
		echo "!! ABORT: the ATOC-trailer read session connected but reported no savebin success" >&2
		echo "   line -- refusing to trust $TRAILER_SECTOR_FILE. Transcript:" >&2
		cat "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.out" >&2
		exit 6
	fi

	ATOC_RESOLVE_OUT="$(mktemp "${TMPDIR:-/tmp}/aen-erase-atoc-resolve.out.XXXXXX")" || exit 6
	if ! PYTHONIOENCODING=utf-8 python3 "$ALP_SDK_DIR/scripts/bench/aen/atoc_trailer.py" resolve \
		--sector-file "$TRAILER_SECTOR_FILE" --sector-base "$(printf '0x%X' "$((TRAILER_SECTOR_BASE))")" \
		--window-lo "$FLOWD_WINDOW_LO" --window-end "$(printf '0x%X' "$ATOC_END")" \
		>"$ATOC_RESOLVE_OUT" 2>&1; then
		echo "!! ABORT: could not determine the resident ATOC package's location -- refusing" >&2
		echo "   the erase rather than guess. Transcript:" >&2
		cat "$ATOC_RESOLVE_OUT" >&2
		exit 6
	fi
	cat "$ATOC_RESOLVE_OUT" >&2

	if grep -q '^NO_ATOC$' "$ATOC_RESOLVE_OUT"; then
		echo ">>> ATOC trailer: no resident package found (blank trailer) -- proceeding." >&2
	else
		ATOC_PACKAGE_START=$(sed -n 's/^PACKAGE_START=//p' "$ATOC_RESOLVE_OUT")
		printf '>>> ATOC trailer: resident package starts at %s\n' "$ATOC_PACKAGE_START" >&2
		if [ "$((ATOC_BASE))" -gt "$((ATOC_PACKAGE_START))" ]; then
			printf '!! ABORT: the erase window ends at %s, but the resident ATOC package starts\n' "$ATOC_BASE" >&2
			printf '   at %s -- ERASING WOULD ZERO PART OF A LIVE, NO-SE-UART-RECOVERABLE BOOT\n' "$ATOC_PACKAGE_START" >&2
			echo "   TABLE. Refusing. This is exactly the alp-sdk#2233 measured hazard." >&2
			exit 7
		fi
		echo ">>> ATOC trailer: resident package does not overlap the erase window -- proceeding." >&2
	fi
fi

# --check-only STOPS HERE (alp-sdk#2233 review round 4) -- the read-only
# DPIDR preflight and ATOC-trailer read/overlap decision above are the
# whole point of this mode; reaching this line without having already
# exited 6 (undeterminable) or 7 (overlap refused) means the real erase
# WOULD proceed. Exits BEFORE the pre-write backup, bench_flowd_prepare_write,
# or any write-session CommandFile below -- none of that has run yet.
if [ "$CHECK_ONLY" = 1 ]; then
	echo ">>> --check-only: read-only trailer check complete -- the erase WOULD PROCEED" >&2
	echo "    (nothing written; no write session was built or run)." >&2
	exit 0
fi

# SECTOR-PAD (alp-sdk#2233): the built-in loader rewrites the WHOLE 16 KiB
# sector(s) a write touches, so a truly generic Flow D writer must read
# neighbours first (bench-env.sh's Flow D section header). This window IS
# sector-aligned at both ends (BASE and END are both 0x4000-multiples --
# asserted by construction above: BASE/END come straight from the preset and
# END is checked adjacent to `atoc`), so the padded image ends up being
# nothing but $ZEROS itself; the machinery is still run for real, not
# special-cased away, so a future preset move that breaks alignment is
# padded correctly instead of silently writing a partial sector wrong.
#
# --dry-run (this script's own flag, hazard-driven: DRY_RUN must open no
# probe at all) implies FLOWD_DRY_RUN for the pad machinery too -- harmless
# here specifically because $ZEROS covers every byte of every sector it
# touches, so no real neighbour content is ever needed to prove this write,
# aligned or not.
if [ "$DRY_RUN" = 1 ]; then
	export FLOWD_DRY_RUN=1
fi
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-erase-storage-XXXXXX")" || exit 5
bench_flowd_prepare_write erase-storage "$FLOWD_SCRATCH" "$ZEROS:$BASE" || exit 5

# STABLE BACKUP LOCATION (alp-sdk#2233 review blocker 2): $FLOWD_SCRATCH is an
# ephemeral mktemp -d nobody will remember to look at, and the pre-read
# sector images it holds are the ONLY restore source on a board with no
# SE-UART. Copy them somewhere printed and stable before writing anything.
# DRY_RUN skips this too (bench_flowd_prepare_write above never touched a
# probe in that mode, so there is nothing real to back up).
if [ "$DRY_RUN" != 1 ]; then
	FLOWD_BACKUP_DIR="$BENCH_ROOT/flowd-backup/$(date -u +%Y%m%dT%H%M%SZ)-erase-storage"
	mkdir -p "$FLOWD_BACKUP_DIR"
	cp -a "$FLOWD_SCRATCH/sectors/." "$FLOWD_BACKUP_DIR/"
	echo ">>> pre-write sector backup: $FLOWD_BACKUP_DIR" >&2
fi

# `verifybin` is deliberately GONE here (#2233): it only ever compared
# against J-Link's own in-process flash cache, never a fresh chip read -- see
# the bench_flowd_proof gate below, which replaces it.
#
# Computed into a variable BEFORE the heredoc (#2233 review item 13c): a
# `$(...)` inline inside a heredoc discards the command's own exit status.
FLOWD_LOADBIN_LINES="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)" || {
	echo "!! bench_flowd_loadbin_lines failed for $FLOWD_MANIFEST -- refusing to write nothing." >&2
	exit 5
}
[ -n "$FLOWD_LOADBIN_LINES" ] || {
	echo "!! bench_flowd_loadbin_lines produced no loadbin line for $FLOWD_MANIFEST -- refusing." >&2
	exit 5
}
# RACE CHECK setup (#2233 review major 4) -- see bench-env.sh's "Pre-read ->
# write RACE detection" section. Especially load-bearing HERE: this is the
# one script in this directory that erases a whole customer window, so a
# race between the pre-read and the write is the highest-consequence case.
FLOWD_PREWRITE_LINES="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/prewrite")"
ERASE_WRITE_JLINK="${TMPDIR:-/tmp}/aen-erase-storage.jlink"
ERASE_WRITE_OUT="${TMPDIR:-/tmp}/aen-erase-storage.out"
cat > "$ERASE_WRITE_JLINK" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
$FLOWD_PREWRITE_LINES
$FLOWD_LOADBIN_LINES
exit
EOF

if [ "$DRY_RUN" = 1 ]; then
	echo "--- DRY RUN: nothing was written, no probe was opened ---"
	cat "$ERASE_WRITE_JLINK"
	exit 0
fi

# 4. Write. Deliberately NO reset and NO `g`: this step must not boot
#    anything. Cold power-cycle by hand afterwards and confirm the SE still
#    finds its ATOC (docs/aen-provisioning.md section 2 listener).
#
# CAPTURE THE WRITE-SESSION STATUS (alp-sdk#2233 review round 3, finding 3):
# every writer used to discard this with a bare `|| true`, so a
# bench_jlink_run() backstop refusal (FLOWD_DRY_RUN set, rc=13 -- see
# bench-env.sh) fell straight through to the connect-failure text grep
# below, which found nothing and let the script carry on into the race
# check/proof as if a real session had run. `write_rc=0; cmd || write_rc=$?`
# (not `if cmd; then ...; fi; rc=$?` -- see bench_flowd_proof's own retry-loop
# fix for exactly why that shape is unsafe) keeps `set -e` from aborting on a
# real connect failure (unchanged, handled by the text grep just below) while
# still capturing bench_jlink_run's OWN exit code for the one case that
# matters here: 13 means NO session ran at all.
write_rc=0
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$ERASE_WRITE_JLINK" \
	> "$ERASE_WRITE_OUT" 2>&1 || write_rc=$?
if [ "$write_rc" -eq 13 ]; then
	echo "!! bench_jlink_run REFUSED the write session -- FLOWD_DRY_RUN backstop (rc=13)." >&2
	echo "   Refusing before any race check, proof, or write claim. Should be unreachable" >&2
	echo "   now that FLOWD_DRY_RUN is treated exactly like --dry-run above; this is" >&2
	echo "   defense in depth." >&2
	exit 13
fi
grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Cortex|Found" \
	"$ERASE_WRITE_OUT" | head -30
echo "----- (full log: $ERASE_WRITE_OUT) -----"
if grep -qi "Could not connect to the target device" "$ERASE_WRITE_OUT"; then
	echo "!! $JLINK_DEVICE_FLASH profile FAILED to connect -- the Alif MRAM loader was"
	echo "   never unlocked. The window was NOT erased."
	exit 2
fi

# RUN THE PROOF BEFORE THE RACE CHECK, PRINT BOTH (alp-sdk#2233 review round
# 3, finding 7): an earlier version ran the race check FIRST and exited 11
# immediately on a detected race, so a proof failure right alongside it was
# NEVER EVEN CHECKED, let alone reported -- an operator investigating a race
# had no idea whether the write also failed to land at all. Both now always
# run and both verdicts are always printed; a detected race still wins the
# final exit code (11) even when the proof itself passed, because the race
# means something ELSE touched this window and the whole picture is suspect
# regardless of what the proof says.
proof_failed=0
if ! bench_flowd_proof erase-storage "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
	echo "!! READ-BACK PROOF FAILED -- the window is NOT uniformly 0x00. Do not ship this SoM."
	proof_failed=1
fi
if [ "$proof_failed" -eq 0 ]; then
	printf 'erased: %s .. 0x%X verified all-0x00 (fresh-session read-back proof)\n' "$BASE" "$END"
fi

race_failed=0
if ! bench_flowd_check_race erase-storage "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/sectors" "$FLOWD_SCRATCH/prewrite" "$ERASE_WRITE_OUT"; then
	echo "!! RACE DETECTED -- restore from $FLOWD_BACKUP_DIR and $FLOWD_SCRATCH/prewrite" >&2
	echo "   before trusting this board. The write HAS already happened." >&2
	race_failed=1
fi

if [ "$race_failed" -eq 1 ]; then
	exit 11
fi
if [ "$proof_failed" -eq 1 ]; then
	exit 3
fi
echo "NEXT (by hand, still owed): cold power-cycle the module and confirm the SE"
echo "     boots clean on the docs/aen-provisioning.md section 2 listener -- the"
echo "     ATOC band was not touched, so the banner must still show the app"
echo "     booting, not 'No ATOC'."
