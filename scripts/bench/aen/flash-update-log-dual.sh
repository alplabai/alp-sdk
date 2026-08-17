#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-dual.sh [--package-only] <hp-build-dir> <he-build-dir>
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives the Alif SETOOLS + JLinkExe over the labgrid-held AEN bench).
# Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# Build the dual-entry ATOC package for examples/connectivity/firmware-update-log
# on E1M-AEN801 / Alif E8:
#   - HP owner:  M55_HP, loadAddress 0x50000000, flags ["load", "boot"]
#   - HE client: M55_HE, loadAddress 0x58000000, flags ["load"]
#
# The default package is app-only so it preserves the board's existing
# DEVICE/firewall policy. Set ALP_AEN_INCLUDE_DEVICE_CONFIG=yes only when
# intentionally replacing that policy. The package is written to MRAM only when
# ALP_CONFIRM_DESTRUCTIVE_FLASH=yes is present. Use --package-only to validate
# the SETOOLS package without touching the board.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

PACKAGE_ONLY=0
if [ "${1:-}" = "--package-only" ]; then
	PACKAGE_ONLY=1
	shift
fi

if [ "$#" -ne 2 ]; then
	echo "usage: $0 [--package-only] <hp-build-dir> <he-build-dir>" >&2
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
JLINK="$(bench_jlink_exe)" || exit $?
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")

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

cat > /tmp/firmware-update-log-dual-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
loadbin $PKG $ATOC_ADDR
verifybin $PKG $ATOC_ADDR
RSetType 2
r
g
exit
EOF

# Write the transcript FIRST, fully, then grep|head it for display (#1488
# finding 5) -- a `... | tee out | grep ... | head -N` pipeline lets `head`
# exit after N lines and SIGPIPE grep, which then closes tee's stdout pipe;
# tee can die from that SIGPIPE before JLinkExe's full transcript (including
# the `Verify successful.` / `Verify failed.` line the gate below depends on)
# is written to disk. Once a genuinely good flash's transcript got truncated
# that way, the absence of "verify successful" in the truncated file would
# read as a hard exit 3 on a board that actually flashed fine.
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-write.jlink \
	> /tmp/firmware-update-log-dual-write.out 2>&1 || true
grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" \
	/tmp/firmware-update-log-dual-write.out | head -40

if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" \
	/tmp/firmware-update-log-dual-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

# GATE ON THE VERIFY RESULT (#1488) -- same defect flash-jlink-hp.sh was fixed
# for under #1343. The `verifybin` above was issued but its outcome was never
# read: the output went to a display-only pipe and the connect check was the
# only thing that could fail this script, so a `Verify failed.` exited 0 and
# reported a good flash.
#
# What this gate actually does -- and does NOT do: `loadbin`, `verifybin`,
# `RSetType 2`, `r`, and `g` are all inside the SAME CommanderScript JLinkExe
# has already finished executing by the time the greps below run, and the
# HP-OWNER entry above carries `"flags": ["load", "boot"]` -- the SE has
# already pin-reset and BOOTED the HP owner, which releases the HE client. So
# this gate can only suppress the false "flash complete" report and the beacon
# read-back (read-update-log-proof.sh) that follows; it does NOT and CANNOT
# prevent the write, or the boot that lets the booted images append to the
# update log. See the exit-3 data-loss caveats below.
if grep -qiE "verify failed|verification failed|mismatch" /tmp/firmware-update-log-dual-write.out; then
	echo "!! VERIFY FAILED -- the bytes on the part do NOT match $PKG." >&2
	grep -iE "verify failed|verification failed|mismatch" /tmp/firmware-update-log-dual-write.out | head -5 >&2
	echo "   Do not treat this board as flashed." >&2
	echo "   DATA LOSS: the board was already pin-reset and released (RSetType 2; r; g" >&2
	echo "   already ran inside the same CommanderScript), so the HP owner has already" >&2
	echo "   booted and released the HE client -- alp_ulog_partition may ALREADY have" >&2
	echo "   been appended to by this unverified package. Re-read the partition from a" >&2
	echo "   known-good state before trusting the update log this run produced." >&2
	exit 3
fi
if ! grep -qi "verify successful" /tmp/firmware-update-log-dual-write.out; then
	echo "!! no verifybin success reported -- treating as FAILED (the verify never ran)." >&2
	echo "   DATA LOSS: the board was already pin-reset and released (RSetType 2; r; g" >&2
	echo "   already ran inside the same CommanderScript), so the HP owner has already" >&2
	echo "   booted and released the HE client -- alp_ulog_partition may ALREADY have" >&2
	echo "   been appended to by this unverified package. Re-read the partition from a" >&2
	echo "   known-good state before trusting the update log this run produced." >&2
	exit 3
fi
echo "verify: verifybin OK ($PKG @ $ATOC_ADDR)" >&2

echo "flash complete; capture labgrid console for HP owner + HE client output" >&2
sleep 3
READBACK_ARGS=()
if grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN=y' "$HE_BD/zephyr/.config" \
	2>/dev/null; then
	READBACK_ARGS=(--expect-hw)
fi
"$HERE/read-update-log-proof.sh" "${READBACK_ARGS[@]}"
