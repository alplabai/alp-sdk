#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-dual.sh [--package-only] [--replace-atoc] <hp-build-dir> <he-build-dir>
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives the Alif SETOOLS + JLinkExe over the labgrid-held AEN bench).
# Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# Build the dual-entry ATOC package for examples/connectivity/firmware-update-log
# on an E1M-AEN module (Alif E8):
#   - HP owner:  M55_HP, loadAddress 0x50000000, flags ["load", "boot"]
#   - HE client: M55_HE, loadAddress 0x58000000, flags ["load"]
#
# The default package is app-only so it preserves the board's existing DEVICE
# policy (SETOOLS keeps DEVICE when a JSON omits it, docs/aen-provisioning.md
# section 4). Set ALP_AEN_INCLUDE_DEVICE_CONFIG=yes only when intentionally
# replacing that policy. Every OTHER resident app entry NOT named HP-OWNER/
# HE-CLIENT is a different matter: the `loadbin` below writes the SAME signed
# ATOC structure `app-write-mram -p` would (docs/debugging-aen.md), which
# REPLACES rather than merges, so a foreign app entry (e.g. an A32 Linux boot
# chain) is silently delisted unless --replace-atoc is passed (alp-sdk#2025 --
# see the GUARD before the write, below). The package is written to MRAM only
# when ALP_CONFIRM_DESTRUCTIVE_FLASH=yes is present. Use --package-only to
# validate the SETOOLS package without touching the board.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

PACKAGE_ONLY=0
REPLACE_ATOC=0
while [ $# -gt 0 ]; do
	case "$1" in
	--package-only) PACKAGE_ONLY=1; shift ;;
	--replace-atoc) REPLACE_ATOC=1; shift ;;
	--) shift; break ;;
	-*) echo "unknown flag: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

if [ "$#" -ne 2 ]; then
	echo "usage: $0 [--package-only] [--replace-atoc] <hp-build-dir> <he-build-dir>" >&2
	exit 2
fi

HP_BD="$(cd "$1" && pwd)"
HE_BD="$(cd "$2" && pwd)"
HP_BIN="$HP_BD/zephyr/zephyr.bin"
HE_BIN="$HE_BD/zephyr/zephyr.bin"

[ -f "$HP_BIN" ] || { echo "missing HP zephyr.bin: $HP_BIN" >&2; exit 2; }
[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns.
JLINK_ARGS=(bench_jlink_run)

# 0. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. This script writes MRAM directly over
# JLinkExe (the `loadbin $PKG $ATOC_ADDR` below) and was the ONLY such writer
# with no DPIDR gate, unlike flash-jlink.sh / flash-jlink-hp.sh /
# flash-jlink-mramxip.sh which have carried one (alp-sdk#1318). JLINK_SN
# narrows probe choice but does not itself prove which board answered, and on
# alplab-gw the AEN E8 and the V2N-M1 GD32 share a cloned OEM serial. Hard
# ABORT, not a warning -- read-only connect first, no writes until confirmed.
#
# AEN_DPIDR/GD32_DPIDR come from bench-env.sh, which is the single source for
# both IDs -- do not re-declare them here.
cat > /tmp/firmware-update-log-dual-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-preflight.jlink \
  > /tmp/firmware-update-log-dual-preflight.out 2>&1 || true
bench_jlink_assert_aen_dpidr /tmp/firmware-update-log-dual-preflight.out "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

check_itcm_vector() {
	local role="$1"
	local bin="$2"
	local rv

	rv=$(xxd -e -l 8 "$bin" | awk '{print $3}')
	echo ">>> $role reset vector: 0x$rv" >&2
	case "$rv" in
		000*) : ;;
		800*) echo "!! $role is MRAM-linked, not ITCM-loadable for the dual ATOC" >&2; exit 3 ;;
		*) echo "!! $role reset vector is unexpected for an ITCM-loaded AEN image" >&2; exit 3 ;;
	esac
}

check_itcm_vector "HP" "$HP_BIN"
check_itcm_vector "HE" "$HE_BIN"

HP_IMG=firmware-update-log-hp.bin
HE_IMG=firmware-update-log-he.bin
cp -f "$HP_BIN" "$SET/build/images/$HP_IMG"
cp -f "$HE_BIN" "$SET/build/images/$HE_IMG"

{
	echo "{"
	if [ "${ALP_AEN_INCLUDE_DEVICE_CONFIG:-no}" = "yes" ]; then
		echo '    "DEVICE":   { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },'
		echo ">>> including DEVICE config in update-log dual ATOC (ALP_AEN_INCLUDE_DEVICE_CONFIG=yes)" >&2
	else
		echo ">>> app-only update-log dual ATOC; preserving existing DEVICE/firewall policy" >&2
	fi
	cat <<JSON
    "HP-OWNER": { "disabled": false, "binary": "$HP_IMG", "version": "1.0.0", "signed": true,
                  "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] },
    "HE-CLIENT": { "disabled": false, "binary": "$HE_IMG", "version": "1.0.0", "signed": true,
                   "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load"] }
}
JSON
} > "$SET/build/config/firmware-update-log-dual.json"

cd "$SET"
echo ">>> AEN firmware-update-log dual-entry ATOC" >&2
./app-gen-toc -f build/config/firmware-update-log-dual.json >/tmp/firmware-update-log-dual-gentoc.log 2>&1 \
	|| { echo "gen-toc FAILED"; tail -20 /tmp/firmware-update-log-dual-gentoc.log; exit 1; }

PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address" >&2; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ATOC_ADDR" >&2

if [ "$PACKAGE_ONLY" -eq 1 ]; then
	echo "package-only: not flashing MRAM" >&2
	exit 0
fi

if [ "${ALP_CONFIRM_DESTRUCTIVE_FLASH:-}" != "yes" ]; then
	echo "refusing destructive MRAM flash: set ALP_CONFIRM_DESTRUCTIVE_FLASH=yes for this run" >&2
	exit 4
fi

# GUARD (alp-sdk#2025) -- see bench_atoc_replace_guard in bench-env.sh.
# HP-OWNER and HE-CLIENT are what THIS run itself is about to (re)write, so
# they are the allowed set -- the guard fires only on a genuinely foreign
# resident entry (e.g. an A32 Linux boot chain), never on this script's own
# output. This helper has no other SE_UART dependency (its write goes over
# JLinkExe, not app-write-mram) -- the guard needs SE_UART only for its own
# read-only `maintenance -opt gettoc` query and reports "unverified" (abort
# unless --replace-atoc) if it is unset, same as any other missing input.
bench_atoc_replace_guard "$REPLACE_ATOC" flash-update-log-dual HP-OWNER HE-CLIENT || exit $?

# SECTOR-PAD (alp-sdk#2233): the built-in loader rewrites the WHOLE 16 KiB
# sector(s) $PKG touches and never reads their prior contents first -- see
# bench-env.sh's Flow D section header. Read those sectors' current MRAM
# content and overlay $PKG on them; the padded image is what gets loadbin'ed.
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-update-log-dual-XXXXXX")" || exit 9
bench_flowd_prepare_write flash-update-log-dual "$FLOWD_SCRATCH" "$PKG:$ATOC_ADDR" || exit 9

# `verifybin` is deliberately GONE here (#2233): it only ever compared
# against J-Link's own in-process flash cache, never a fresh chip read -- see
# the bench_flowd_proof gate below, which runs BEFORE the boot script (#1526
# unchanged: a failed proof must still keep the board from booting an
# unverified image).
#
# Computed into a variable BEFORE the heredoc (#2233 review item 13c): a
# `$(...)` inline inside a heredoc discards the command's own exit status.
FLOWD_LOADBIN_LINES="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)" || {
	echo "!! bench_flowd_loadbin_lines failed for $FLOWD_MANIFEST -- refusing to write nothing." >&2
	exit 9
}
[ -n "$FLOWD_LOADBIN_LINES" ] || {
	echo "!! bench_flowd_loadbin_lines produced no loadbin line for $FLOWD_MANIFEST -- refusing." >&2
	exit 9
}
# RACE CHECK setup (#2233 review major 4) -- see bench-env.sh's "Pre-read ->
# write RACE detection" section.
FLOWD_PREWRITE_LINES="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/prewrite")"
cat > /tmp/firmware-update-log-dual-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
$FLOWD_PREWRITE_LINES
$FLOWD_LOADBIN_LINES
exit
EOF

# FLOWD_DRY_RUN (#2233 review blocker 1a): exit right here, having printed
# what the write session WOULD run, WITHOUT ever invoking JLinkExe on it --
# and, since the boot CommandFile is separate (#1526) and gated on this
# session's own outcome, without reaching the boot script either.
if [ -n "$FLOWD_DRY_RUN" ]; then
	echo "--- DRY RUN: nothing written, no probe was opened (FLOWD_DRY_RUN) ---"
	cat /tmp/firmware-update-log-dual-write.jlink
	exit 10
fi

# Write the transcript FIRST, fully, then grep|head it for display (#1488
# finding 5) -- a `... | tee out | grep ... | head -N` pipeline lets `head`
# exit after N lines and SIGPIPE grep, which then closes tee's stdout pipe;
# tee can die from that SIGPIPE before JLinkExe's full transcript is written
# to disk, and the connect-failure check below depends on the FULL transcript.
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-write.jlink \
	> /tmp/firmware-update-log-dual-write.out 2>&1 || true
grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" \
	/tmp/firmware-update-log-dual-write.out | head -40

if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" \
	/tmp/firmware-update-log-dual-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

# RACE CHECK (#2233 review major 4).
if ! bench_flowd_check_race flash-update-log-dual "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/sectors" "$FLOWD_SCRATCH/prewrite"; then
	echo "!! RACE DETECTED -- restore from $FLOWD_SCRATCH/sectors and $FLOWD_SCRATCH/prewrite" >&2
	echo "   before trusting this board. The write HAS already happened (the boot has not --" >&2
	echo "   #1526 still gates that on the proof below)." >&2
	exit 11
fi

# This gate is LOAD-BEARING (#1526).  The CommanderScript above carries only
# `loadbin`; `RSetType 2` / `r` / `g` moved to a SECOND script that runs
# further down, and only if the check below passes.  So a failed proof now
# stops the board being reset into an image that did not verify -- and
# because the HP-OWNER entry carries `"flags": ["load", "boot"]`, not
# booting is what keeps the HP owner (and the HE client it releases) from
# running and appending to the update log.
#
# The MRAM write itself has of course already happened -- that is what
# `loadbin` is.  What is prevented is acting on it.
#
# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1488 verifybin gate)
# -- a FRESH read-only J-Link session savebin's every padded range back and
# cmp's it byte-for-byte against the padded image, proving both that $PKG
# landed AND that its sector neighbours survived THIS write -- NOT a
# persistence proof across a power cycle.
if ! bench_flowd_proof flash-update-log-dual "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
	echo "!! READ-BACK PROOF FAILED -- MRAM does NOT match the padded image for $PKG @ $ATOC_ADDR." >&2
	echo "   Do not treat this board as flashed." >&2
	echo "   The board was NOT reset or booted (#1526): the reset/boot CommanderScript" >&2
	echo "   runs only past this gate, so neither the HP owner nor the HE client ran," >&2
	echo "   and alp_ulog_partition is intact.  MRAM now holds an image that failed" >&2
	echo "   proof -- reflash before booting this board." >&2
	exit 3
fi
echo "verify: read-back proof OK ($PKG @ $ATOC_ADDR, sector-padded; not a cold-cycle persistence proof)" >&2

# ONLY NOW reset into the image (#1526).  Separate CommanderScript so the boot
# is genuinely downstream of the verify result -- inside one script JLinkExe
# runs everything before the shell can read anything, which is what made the
# old gate advisory.
cat > /tmp/firmware-update-log-dual-boot.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
RSetType 2
r
g
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-boot.jlink 	> /tmp/firmware-update-log-dual-boot.out 2>&1 || true
if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" 	/tmp/firmware-update-log-dual-boot.out; then
	echo "!! reset/boot script failed to connect -- image is verified in MRAM but the" >&2
	echo "   board was not booted; alp_ulog_partition is untouched." >&2
	exit 2
fi

echo "flash complete; capture labgrid console for HP owner + HE client output" >&2
sleep 3
READBACK_ARGS=()
if grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN=y' "$HE_BD/zephyr/.config" \
	2>/dev/null; then
	READBACK_ARGS=(--expect-hw)
fi
"$HERE/read-update-log-proof.sh" "${READBACK_ARGS[@]}"
