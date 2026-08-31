#!/usr/bin/env bash
# scripts/bench/aen/erase-storage.sh [--dry-run]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh; drives
# JLinkExe, a Linux binary on this bench). Run it under WSL2 on Windows; macOS
# has the J-Link tools but is not the bench host. See docs/aen-provisioning.md.
#
# PROVISIONING STEP (alp-sdk#1430) -- erase the E1M-AEN801 CUSTOMER STORAGE
# WINDOW before a SoM ships, so the module does not leave manufacturing
# carrying a previous application's image in the window the customer's first
# NVS write lands in.
#
# WHY: alp-sdk#1334 measured, on E8 silicon, ~110 KiB of a stale
# previously-flashed Zephyr application image sitting in what was then the
# `storage` partition. It is not live data -- but a customer who dumps the part
# sees another application's shell strings, and a first NVS write silently
# destroys bytes that look meaningful. #1334 closed on the measurement; #1430
# is the standing "erase before ship" step it left behind.
#
# [BENCH-GATED / UNVERIFIED-ON-BENCH] This script has NEVER been run against a
# module. Everything below is assembled from facts already established in this
# repo (cited inline); the run itself, its transcript, and the post-erase cold
# power-cycle are still owed. Do not record a SoM as erased on the strength of
# this file.
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
#    `storage` ends (metadata/e1m_modules/E1M-AEN801.yaml). SETOOLS top-anchors
#    the signed ATOC there and grows it downward, so an overshoot of even one
#    byte can land in the live ATOC -- the board then boots to `No ATOC` and
#    needs re-provisioning over the SE-UART (docs/aen-provisioning.md section 4).
#    The window is therefore DERIVED from the preset and asserted adjacent to
#    `atoc` below; nothing here is a hardcoded address.
#
# 5. WRONG-BOARD RISK. This bench has three J-Links and two share OEM serial
#    603000869 (see bench-env.sh). Zeroing an MRAM range on the wrong target
#    corrupts a different, live board. The DPIDR gate is a hard abort, not a
#    warning.
#
# Address-range note: `loadbin` into MRAM is bench-established at 0x80010000
# (docs/aen-provisioning.md section 0.5 Option B) and at the ATOC package
# address near the top of the window (docs/aen-bench-bringup.md, the "Burning:"
# address) -- i.e. below and above this range -- but the storage window itself
# has NOT been written this way on the bench. That is part of what the first
# run has to prove.
#
# Exit codes (aligned with the sibling Flow D helpers):
#   0  window written and byte-verified as erased
#   2  the part-number device profile could not connect (nothing written)
#   3  verify failed, or no verify result at all -- do NOT treat as erased
#   4  DPIDR gate: this is not the AEN E8 (nothing written)
#   5  the storage window could not be derived from the preset, or it is not
#      adjacent to `atoc` (nothing written)
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

if [ "$DRY_RUN" = 1 ]; then
	JLINK="$(bench_jlink_exe 2>/dev/null || echo JLinkExe)"
else
	JLINK="$(bench_jlink_exe)" || exit $?
fi
JLINK_ARGS=("$JLINK")
# See ram-run.sh for why the selector is conditional on JLINK_SN.
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")

# 1. DERIVE the window from the SoM preset -- single source of truth, so a
#    future layout move cannot leave a stale address baked in here. Then assert
#    it ends exactly where the SE-owned `atoc` band begins (hazard 4).
PRESET="$ALP_SDK_DIR/metadata/e1m_modules/E1M-AEN801.yaml"
if [ ! -r "$PRESET" ]; then
	echo "!! ABORT: cannot read $PRESET -- run this from inside the alp-sdk checkout." >&2
	exit 5
fi

# <region-name> <field> -> the field's value off that memory_map row.
_region_field() {
	sed -n "s/.*name:[[:space:]]*$1,.*[[:space:]]$2:[[:space:]]*\([0-9a-fA-Fx]*\).*/\1/p" "$PRESET" | head -1
}
BASE=$(_region_field storage base)
KIB=$(_region_field storage size_kib)
ATOC_BASE=$(_region_field atoc base)
if [ -z "$BASE" ] || [ -z "$KIB" ] || [ -z "$ATOC_BASE" ]; then
	echo "!! ABORT: could not parse the storage/atoc memory_map regions out of" >&2
	echo "   $PRESET -- refusing to guess an MRAM address. Fix the parse, or the" >&2
	echo "   preset moved (storage=$BASE size_kib=$KIB atoc=$ATOC_BASE)." >&2
	exit 5
fi
SIZE=$((KIB * 1024))
END=$((BASE + SIZE))
if [ "$END" -ne $((ATOC_BASE)) ]; then
	printf '!! ABORT: storage window %s + %s KiB ends at 0x%X, but atoc starts at %s.\n' \
		"$BASE" "$KIB" "$END" "$ATOC_BASE" >&2
	echo "   The layout moved. Writing this range could land in the SE-owned ATOC" >&2
	echo "   band and leave the board at 'No ATOC'. Re-read the preset first." >&2
	exit 5
fi
printf '>>> customer storage window: %s .. 0x%X (%s KiB, exclusive of atoc at %s)\n' \
	"$BASE" "$END" "$KIB" "$ATOC_BASE" >&2

# 2. Build the erased pattern. 0x00 IS the erased value on this MRAM (hazard 2),
#    so the file is literally $SIZE zero bytes -- and it doubles as the
#    verifybin reference, which makes "erased" a byte-compare rather than a
#    claim.
ZEROS=/tmp/aen-storage-erased.bin
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
#    This runs BEFORE the erase CommanderScript is written, not after: a failed
#    gate then leaves no destructive script staged in /tmp for someone to run by
#    hand.  It also keeps the erase script's `verifybin` adjacent to its own
#    transcript, which is what tests/scripts/test_bench_jlink_connect_guard.py
#    checks -- with the preflight in between, that pairing resolved to the
#    PREFLIGHT transcript and the gate below looked absent.
#
#    DRY_RUN never reaches here, so it still opens no probe.
if [ "$DRY_RUN" != 1 ]; then
	cat > /tmp/aen-erase-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/aen-erase-preflight.jlink \
		> /tmp/aen-erase-preflight.out 2>&1 || true
	bench_jlink_assert_aen_dpidr /tmp/aen-erase-preflight.out "storage-erase preflight" || exit 4
	echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2
fi

cat > /tmp/aen-erase-storage.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
loadbin $ZEROS_FOR_JLINK $BASE
verifybin $ZEROS_FOR_JLINK $BASE
exit
EOF

if [ "$DRY_RUN" = 1 ]; then
	echo "--- DRY RUN: nothing was written, no probe was opened ---"
	cat /tmp/aen-erase-storage.jlink
	exit 0
fi

# 4. Write + verify. Deliberately NO reset and NO `g`: this step must not boot
#    anything. Cold power-cycle by hand afterwards and confirm the SE still
#    finds its ATOC (docs/aen-provisioning.md section 2 listener). Transcript is
#    written in full FIRST, then grepped for display -- a `| tee | grep | head`
#    pipeline can SIGPIPE tee and truncate away the very `Verify` line the gate
#    below reads (#1488 finding 5, see flash-jlink.sh).
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/aen-erase-storage.jlink \
	> /tmp/aen-erase-storage.out 2>&1 || true
grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Cortex|Found" \
	/tmp/aen-erase-storage.out | head -30
echo "----- (full log: /tmp/aen-erase-storage.out) -----"
if grep -qi "Could not connect to the target device" /tmp/aen-erase-storage.out; then
	echo "!! $JLINK_DEVICE_FLASH profile FAILED to connect -- the Alif MRAM loader was"
	echo "   never unlocked. The window was NOT erased."
	exit 2
fi

# GATE ON THE VERIFY RESULT (#1488). An unread verifybin is the defect
# flash-jlink.sh and flash-jlink-hp.sh were both fixed for: JLinkExe exits 0
# on a failed verify, so without this the script would report a clean erase
# over a window that still holds the old image.
if grep -qiE "verify failed|verification failed|mismatch" /tmp/aen-erase-storage.out; then
	echo "!! VERIFY FAILED -- the window is NOT uniformly 0x00. Do not ship this SoM."
	grep -iE "verify failed|verification failed|mismatch" /tmp/aen-erase-storage.out | head -5
	exit 3
fi
if ! grep -qi "verify successful" /tmp/aen-erase-storage.out; then
	echo "!! no verifybin success reported -- treating as NOT erased (the verify never ran)."
	exit 3
fi
printf 'erased: %s .. 0x%X verified all-0x00\n' "$BASE" "$END"
echo "NEXT (by hand, still owed): cold power-cycle the module and confirm the SE"
echo "     boots clean on the docs/aen-provisioning.md section 2 listener -- the"
echo "     ATOC band was not touched, so the banner must still show the app"
echo "     booting, not 'No ATOC'."
