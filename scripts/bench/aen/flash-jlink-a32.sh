#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-a32.sh <bl32.bin> <dtb> <xipImage> <cramfs> <atoc-config.json>
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh; drives
# JLinkExe + the Alif SETOOLS, both Linux binaries on this bench). Runs under
# WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- A32 (Cortex-A32 Linux) FIVE-BLOB variant.
#
# Every other helper here writes at most TWO blobs (an app + the signed ATOC).
# The Linux chain measured on E1M-AEN803 silicon on 2026-09-05 needs five, and
# only two of them are described by the ATOC:
#
#   0x80002000  bl32.bin (TF-A SP_MIN, AArch32, XIP)   ATOC: BOOTLOAD
#   0x80010000  carrier .dtb                           NOT in the ATOC
#   0x80020000  xipImage (kernel, XIP from MRAM)       ATOC: A32_APP
#   0x80380000  rootfs.cramfs                          NOT in the ATOC
#   <parsed>    AppTocPackage.bin                      (the ATOC itself)
#
# The dtb reaches the kernel in r2 via TF-A's ARM_PRELOADED_DTB_BASE and the
# cramfs is found through the MTD physmap, so the Secure Enclave inspects
# neither -- which is exactly why a pure SETOOLS/SE-UART flash (Flow A) CANNOT
# place this chain: app-write-mram writes only what the ATOC describes. Both
# boards that boot Linux were flashed this way (J-Link MRAM loader).
#
# The chain as measured ends exactly at 0x80580000, the 5.5 MB MRAM top; the
# blob-vs-blob overlap check in step 2 below is derived from the addresses
# above plus the per-build ATOC address, not from a hardcoded total.
#
# MEASURED, 2026-09-05: Linux 6.12.6 boots to an interactive shell on the Alif
# Ensemble E8 (AE822) Cortex-A32 on two E1M-AEN803 modules with this layout.
# Chain: Secure Enclave -> TF-A BL32 (SP_MIN, AArch32) -> Linux as BL33. No
# BL1/BL2/BL31 and no U-Boot. Kernel RAM is on-chip SRAM (7528 kB).
# Upstream sources for the inputs (this script builds none of them):
#   kernel  github.com/alifsemi/linux_alif            branch v6.12-dev
#                                                     (branch main is an EMPTY
#                                                      placeholder)
#   TF-A    github.com/alifsemi/trusted-firmware-a_alif
#                                                     branch alif_lts-v2.10.8,
#                                                     PLAT=devkit_e7 with
#                                                     ALIF_SOC_E8=1 (default 1;
#                                                     covers both E7 and E8)
#
# NOT TRUE, do not read this script as implying otherwise: no bitbake/Yocto
# build has ever been run for this chain (MACHINE e1m-aen801-a32 is still a
# non-buildable placeholder, #1968/#1971); the 64 MB external HyperRAM at
# 0xa0000000 is NOT usable (Alif TF-A ships PSRAM init for AP Memory and ISSI
# only and the fitted part is a Winbond W958D8NBYA5I, #1970, being measured);
# the OSPI0 chip-select assignment is UNMEASURED (#1973) and the HyperRAM
# capacity is disputed (#1969).
#
# ---------------------------------------------------------------------------
# SEQUENCING THAT MATTERS -- all of it measured on this silicon
# ---------------------------------------------------------------------------
#
# 1. DO NOT RESET BEFORE PROGRAMMING. With RSetType 2, an `r` makes AP[3]
#    (APAddr 0x00300000) VANISH from the CoreSight scan -- measured 8 times
#    out of 8 -- and after that nothing can be halted. So this script issues
#    no reset at all: plain `connect`, then `h`. The board is power-cycled by
#    hand afterwards (see ACCEPTANCE below), which is also the read-back the
#    flash must survive.
#
# 2. GATE THE HALT ON THE REGISTER DUMP, NOT ON THE BANNER. `h` prints a
#    register dump ONLY when the core genuinely halted; the line to look for
#    is `PC = <8 hex>, CycleCnt = `. "Cortex-M55 identified." is printed at
#    EVERY connect whether the halt worked or not, so gating on it passes a
#    run that never halted. SES-parked PC values seen here: PC = 0000000C and
#    PC = 00000ED2.
#
# 3. `exec SetSkipProgOnCRCMatch = 0` so J-Link never skips a program after a
#    debug read decided the contents already match.
#
# 4. ALL FIVE loadbins IN ONE HELD SESSION, `, noreset` on each, no reset
#    between them.
#
# 5. EVERY BLOB IS PASSED WITH A .bin FILENAME. J-Link `loadbin` REFUSES a
#    .dtb: it prints `File is of unknown / unsupported format.`, emits NO
#    `Flash download:` line, and writes nothing -- a SILENT no-op that only an
#    explicit verify catches. This script therefore stages a .bin copy of every
#    input under $WORK (step 2) instead of trusting the caller's extensions,
#    and step 4 fails the run when any loadbin produced no `Flash download:`
#    line, for exactly that reason.
#
# ---------------------------------------------------------------------------
# ACCEPTANCE -- this script cannot perform it
# ---------------------------------------------------------------------------
# `verifybin` alone is NOT acceptance. There is a documented precedent on this
# hardware of "Verify successful." followed by a COLD POWER CYCLE that reverted
# the write. Acceptance is: cold power-cycle the board, then read the bytes
# back (re-verify the staged files, and confirm the SE boot table shows
# BOOTLOAD/A32_APP where you put them). The script prints the exact staged
# paths at the end so the post-cycle read-back can use the same references.
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export
# SETOOLS_DIR (and obtain SETOOLS from Alif) before running this. See README.md.
#
# Exit codes (aligned with the sibling Flow D helpers):
#   0  all five blobs written and all five verifybin passes reported success
#   1  usage, a missing/empty input, or app-gen-toc / ATOC-guard failure
#      (nothing written)
#   2  the part-number device profile could not connect (nothing written)
#   3  verify failed, or fewer than 5 of 5 verifybin passes reported success
#   4  DPIDR gate: this is not the AEN E8 (nothing written)
#   5  a loadbin produced no `Flash download:` line -- it wrote NOTHING
#   6  the core never halted (no `PC = ..., CycleCnt = ` register dump)
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

if [ "$#" -ne 5 ]; then
	echo "usage: $(basename "$0") <bl32.bin> <dtb> <xipImage> <rootfs.cramfs> <atoc-config.json>" >&2
	echo "  atoc-config.json: an ATOC config for app-gen-toc. A path is copied into" >&2
	echo "  SETOOLS build/config; a bare filename is taken as already staged there." >&2
	echo "  It must describe BOOTLOAD (A32_0 @ 0x80002000) and A32_APP (A32_0 @" >&2
	echo "  0x80020000); the dtb and the cramfs are deliberately NOT in it." >&2
	exit 1
fi

BL32="$1"
DTB="$2"
KERNEL="$3"
CRAMFS="$4"
ATOC_CFG="$5"

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
JLINK="$(bench_jlink_exe)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
# Select the AEN J-Link by serial, same rule as flash-jlink-mramxip.sh: NO
# hardcoded serial default (a bench-wide serial is SHARED by two probes that
# differ only by USB path, so a silent default can pick the WRONG board).
# Export JLINK_SN yourself to disambiguate -- but it is the DPIDR gate below,
# not the serial, that stops a write to the wrong target.
SEL="${JLINK_SN:+SelectEmuBySN $JLINK_SN}"

WORK=/tmp/flowd-a32-stage
rm -rf "$WORK"
mkdir -p "$WORK"

# 1. SAFETY GATE -- confirm we are talking to the AEN E8 before any MRAM write.
# Read-only connect through the generic device profile; no reset (see
# SEQUENCING 1). AEN_DPIDR is set unconditionally by bench-env.sh (#1716) and
# is deliberately NOT overridable here.
cat > /tmp/flowd-a32-preflight.jlink <<EOF
$SEL
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-a32-preflight.jlink \
	> /tmp/flowd-a32-preflight.out 2>&1 || true
bench_jlink_assert_aen_dpidr /tmp/flowd-a32-preflight.out "A32 MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# 2. Build the ATOC, then stage every blob as a .bin (see SEQUENCING 5).
case "$ATOC_CFG" in
	*/*) cp -f "$ATOC_CFG" "$SET/build/config/$(basename "$ATOC_CFG")" ;;
esac
CFG_NAME="$(basename "$ATOC_CFG")"
[ -s "$SET/build/config/$CFG_NAME" ] || {
	echo "!! ATOC config '$CFG_NAME' not found (or empty) under $SET/build/config" >&2
	exit 1
}
# #1069/#1981 window/overlap guard. It accepts A32_0 entries (base bound
# 0x80002000, ceiling MRAM_END 0x80580000) and rejects an ATOC that stages
# both an A32 mramAddress entry and an M55 slot0 entry -- the A32 chain covers
# both M55 slot0 windows in full, so those are mutually exclusive.
python3 "$ALP_SDK_DIR/scripts/aen_atoc.py" "$SET/build/config/$CFG_NAME" || exit 1

(cd "$SET" && ./app-gen-toc -f "build/config/$CFG_NAME") >/tmp/gentoc-a32.log 2>&1 \
	|| { echo "gen-toc FAILED"; tail -20 /tmp/gentoc-a32.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' "$SET/build/app-package-map.txt" | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }

# Insist on the 0x form before this value is used as BOTH a printf '%d'
# operand and a raw J-Link address. Without the prefix `printf '%d'` treats it
# as decimal -- or, for a hex-looking string, errors to stderr and yields 0,
# which would leave the overlap check below comparing 0 against 0 and passing
# an oversized blob instead of rejecting it. A silent pass here is exactly the
# failure this script exists to prevent.
case "$ATOC_ADDR" in
	0x[0-9a-fA-F]*)
		;;
	*)
		echo "!! APP Package Start Address '$ATOC_ADDR' is not 0x-prefixed hex"
		echo "   (parsed from $SET/build/app-package-map.txt)"
		exit 1
		;;
esac

# Parallel arrays, in write order. LIMIT is where the NEXT blob starts, so an
# oversized blob is caught before it silently overwrites its neighbour; the
# cramfs limit is the per-build ATOC address (it moves with package size) and
# the ATOC's own limit is the 0x80580000 MRAM top.
LABELS=(bl32 dtb xipImage cramfs atoc)
SRCS=("$BL32" "$DTB" "$KERNEL" "$CRAMFS" "$PKG")
ADDRS=(0x80002000 0x80010000 0x80020000 0x80380000 "$ATOC_ADDR")
LIMITS=(0x80010000 0x80020000 0x80380000 "$ATOC_ADDR" 0x80580000)
STAGED=()
for i in "${!LABELS[@]}"; do
	src="${SRCS[$i]}"
	[ -s "$src" ] || { echo "!! ${LABELS[$i]}: '$src' is missing or empty"; exit 1; }
	dst="$WORK/${LABELS[$i]}.bin"
	cp -f "$src" "$dst"
	STAGED+=("$dst")
	sz=$(stat -c%s "$dst")
	end=$(( $(printf '%d' "${ADDRS[$i]}") + sz ))
	lim=$(printf '%d' "${LIMITS[$i]}")
	if [ "$end" -gt "$lim" ]; then
		printf '!! %s: %d B at %s ends at 0x%x, past 0x%x (the next blob)\n' \
			"${LABELS[$i]}" "$sz" "${ADDRS[$i]}" "$end" "$lim"
		exit 1
	fi
	printf '    %-8s -> %s (%d B, from %s)\n' "${LABELS[$i]}" "${ADDRS[$i]}" "$sz" "$src" >&2
done

# 3. One held session: connect, halt, disable CRC-skip, five loadbins, five
#    verifybins. No reset anywhere -- see SEQUENCING 1.
{
	echo "$SEL"
	echo "si SWD"
	echo "speed $JLINK_SPEED"
	echo "device $DEV"
	echo "connect"
	echo "h"
	echo "exec SetSkipProgOnCRCMatch = 0"
	for i in "${!STAGED[@]}"; do
		echo "loadbin ${STAGED[$i]} ${ADDRS[$i]}, noreset"
	done
	for i in "${!STAGED[@]}"; do
		echo "verifybin ${STAGED[$i]} ${ADDRS[$i]}"
	done
	echo "exit"
} > /tmp/flowd-a32.jlink
$JLINK -nogui 1 -CommanderScript /tmp/flowd-a32.jlink 2>&1 | tee /tmp/flowd-a32.out | \
	grep -iE "could not connect|fail|error|unsupported format|Verify|Flash download|PC = |Cortex" | head -40
echo "----- (full log: /tmp/flowd-a32.out) -----"

if grep -qi "Could not connect to the target device" /tmp/flowd-a32.out; then
	echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe."; exit 2
fi

# 4a. HALT GATE. Only the register dump proves the halt (SEQUENCING 2).
if ! grep -qE 'PC = [0-9A-Fa-f]{8}, CycleCnt = ' /tmp/flowd-a32.out; then
	echo "!! core never halted -- no 'PC = <8 hex>, CycleCnt = ' register dump in the"
	echo "   transcript. 'Cortex-M55 identified.' is NOT proof of a halt; it prints at"
	echo "   every connect. Do not treat this board as flashed; re-run without any"
	echo "   intervening reset (an 'r' with RSetType 2 makes AP[3] vanish, 8/8)."
	exit 6
fi

# 4b. SILENT-NO-OP GATE. A refused loadbin (the .dtb case) writes nothing and
# prints no `Flash download:` line, while JLinkExe still exits 0 -- so pair each
# echoed `loadbin` command with the download line that must follow it.
# ponytail: pairs by "next loadbin prompt closes the previous one", which is
# text-order only; tighten to per-file matching if a J-Link version ever
# interleaves the two.
missing=$(awk '
	/^J-Link>loadbin /   { if (pending) print cmd; pending = 1; cmd = $0; next }
	/Flash download:/    { pending = 0 }
	/^J-Link>verifybin / { if (pending) { print cmd; pending = 0 } }
	END                  { if (pending) print cmd }
' /tmp/flowd-a32.out)
if [ -n "$missing" ]; then
	echo "!! a loadbin produced NO 'Flash download:' line -- it wrote NOTHING:"
	echo "$missing" | awk '{ print "     " $0 }'
	echo "   J-Link refuses a file whose name it does not recognise as binary"
	echo "   ('File is of unknown / unsupported format.') and still exits 0."
	exit 5
fi

# 4c. VERIFY GATE (#1343/#1488 shape). Both directions: an explicit failure
# string, and the COUNT -- a run that aborted before the verifies executed
# reports neither, and a "no news is good news" gate would pass it. A
# "Verification failed" printed by loadbin itself is its INTERNAL verify and is
# NOT authoritative; these five verifybin passes are.
if grep -qiE "verify failed|verification failed|mismatch" /tmp/flowd-a32.out; then
	echo "!! VERIFY FAILED -- the bytes on the part do NOT match the images."
	grep -iE "verify failed|verification failed|mismatch" /tmp/flowd-a32.out | head -5
	echo "   Do not treat this board as flashed."
	exit 3
fi
verify_ok=$(grep -ci "verify successful" /tmp/flowd-a32.out || true)
if [ "${verify_ok:-0}" -lt 5 ]; then
	echo "!! only ${verify_ok:-0} of 5 verifybin passes reported success -- treating as FAILED."
	echo "   (expected one per loadbin: bl32, dtb, xipImage, cramfs, AppTocPackage.)"
	exit 3
fi
echo "verify: ${verify_ok}/5 verifybin passes OK (bl32 + dtb + xipImage + cramfs + AppTocPackage)"

echo
echo "NOT ACCEPTANCE YET. Cold power-cycle the board, then read the bytes back --"
echo "there is a documented precedent on this hardware of 'Verify successful.'"
echo "followed by a cold cycle reverting the write. Re-verify the staged files"
echo "(kept for exactly this) through the part profile, and confirm the SE boot"
echo "table lists BOOTLOAD and A32_APP at the addresses above:"
for i in "${!STAGED[@]}"; do
	echo "    verifybin ${STAGED[$i]} ${ADDRS[$i]}"
done
