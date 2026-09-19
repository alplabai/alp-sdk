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
# [BENCH-VERIFIED 2026-08-30] Run once against a real module (off-labgrid
# E1M-AEN801, `AE822FA0E5597LS0`, J-Link `000821005680`): the window
# 0x80560000 .. 0x80578000 verified all-0x00, and a cold power-cycle showed
# `u VB` on the `ALP-HE` boot row, so the ATOC band was undisturbed by the
# erase. Transcript in docs/aen-provisioning.md section 7. One bench run is
# not a standing guarantee for a different module -- the DPIDR gate and
# verifybin below still run, and must still pass, on every unit.
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
# address) -- i.e. below and above this range -- and the storage window
# itself was written this way and byte-verified on 2026-08-30 (see the
# [BENCH-VERIFIED] note above).
#
# Exit codes (aligned with the sibling Flow D helpers):
#   0  window written and byte-verified (fresh-session read-back proof) as erased
#   2  the part-number device profile could not connect (nothing written)
#   3  the read-back proof failed -- do NOT treat as erased
#   4  DPIDR gate: this is not the AEN E8 (nothing written)
#   5  the storage window could not be derived from the preset, or it is not
#      adjacent to `atoc` (nothing written); or the sector-pad prepare step
#      (alp-sdk#2233 -- plan/pre-read/build) itself refused or failed
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns. DRY_RUN never
# reaches a call site below, so this never opens a probe in that mode.
JLINK_ARGS=(bench_jlink_run)

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
#    so the file is literally $SIZE zero bytes -- and it is the blob the
#    sector-pad machinery below both writes and later read-back-proves,
#    which makes "erased" a byte-compare rather than a claim.
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
#    This runs BEFORE any other probe touch (including the sector pre-read
#    below) and before the erase CommanderScript is written: a failed gate
#    then leaves no destructive script staged in /tmp for someone to run by
#    hand, and nothing has read from a possibly-wrong board either.
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

# `verifybin` is deliberately GONE here (#2233): it only ever compared
# against J-Link's own in-process flash cache, never a fresh chip read -- see
# the bench_flowd_proof gate below, which replaces it.
cat > /tmp/aen-erase-storage.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)
exit
EOF

if [ "$DRY_RUN" = 1 ]; then
	echo "--- DRY RUN: nothing was written, no probe was opened ---"
	cat /tmp/aen-erase-storage.jlink
	exit 0
fi

# 4. Write. Deliberately NO reset and NO `g`: this step must not boot
#    anything. Cold power-cycle by hand afterwards and confirm the SE still
#    finds its ATOC (docs/aen-provisioning.md section 2 listener).
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

# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1488 verifybin
# gate): a FRESH read-only J-Link session savebin's the whole window back and
# cmp's it byte-for-byte against the padded (here: pure-zero) image.
if ! bench_flowd_proof erase-storage "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
	echo "!! READ-BACK PROOF FAILED -- the window is NOT uniformly 0x00. Do not ship this SoM."
	exit 3
fi
printf 'erased: %s .. 0x%X verified all-0x00 (fresh-session read-back proof)\n' "$BASE" "$END"
echo "NEXT (by hand, still owed): cold power-cycle the module and confirm the SE"
echo "     boots clean on the docs/aen-provisioning.md section 2 listener -- the"
echo "     ATOC band was not touched, so the banner must still show the app"
echo "     booting, not 'No ATOC'."
