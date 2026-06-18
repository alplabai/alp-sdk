#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-hp.sh <build-dir> [sram0_beacon_addr_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh; drives
# JLinkExe + the Alif SETOOLS, both Linux binaries on this bench). Runs under
# WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D, but for the M55-HP (RTSS-HP) core -- the SECOND M55.  Every other
# helper here targets the HE core (cpu_id M55_HE, loadAddress 0x58000000); this
# one authors an M55_HP ATOC (cpu_id M55_HP, loadAddress 0x50000000 = the HP ITCM
# global base) so the SES releases + boots the HP core, which is held in reset at
# power-on (the J-Link AP map shows only the HE core's AP with a readable CPUID).
#
# Observation: the bench reads memory over SWD via the HE/AXI debug AP, not the
# HP core's AP, so this does NOT depend on the HP RAM console being reachable.
# The HP app (examples/aen/aen-hp-core-smoke) writes a LIVENESS BEACON to global
# SRAM0 (0x02000000, always-on, master-agnostic): [0]=magic 0xA11FE000,
# [1]=CPUID, [2]=VTOR, [3]=heartbeat.  This helper reads the beacon, then re-reads
# the heartbeat word -- an ADVANCING heartbeat proves the HP core is actively
# executing (not a stale value from a prior image).
#
# GOTCHA -- returning to the canonical self-test: an HP ATOC becomes the active
# boot image, so the SES boots HP instead of the slot0 person_detect.  After HP
# bring-up, re-flash the canonical image:
#   flash-jlink-mramxip.sh <person_detect-build-dir>
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
BEACON="${2:-0x02000000}"
bench_require_setools || exit $?
SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"

# 1. stage the HP image + an M55_HP signed-ATOC config (cpu_id/loadAddress are
#    the HP core's, the one structural difference from the HE flash configs).
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "HP-APP":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] }
}
JSON

cd "$SET"
echo ">>> FLOW-D M55_HP flash $NAME  (SRAM0 beacon=$BEACON)" >&2
./app-gen-toc -f "build/config/$NAME.json" >/tmp/hp-gentoc.log 2>&1 || { echo "gen-toc FAILED"; tail /tmp/hp-gentoc.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ADDR" >&2

# 2. part-number device unlocks the MRAM loader; write + verify the package, then
#    PIN reset so the SE boot ROM reloads + boots the HP ATOC.
cat > /tmp/hp-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
loadbin $PKG $ADDR
verifybin $PKG $ADDR
RSetType 2
r
g
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/hp-write.jlink 2>&1 | tee /tmp/hp-write.out | \
  grep -iE "could not connect|fail|error|Verify|O\.K\.|Reset" | head -20
if grep -qi "Could not connect to the target device" /tmp/hp-write.out; then
  echo "!! $JLINK_DEVICE_FLASH profile FAILED to connect -- flow D not unlocked on this probe."
  exit 2
fi

# 3. SES has booted the HP core; read the SRAM0 beacon via the generic device
#    (the HE/system AP reads global SRAM0 regardless of HP core state), then
#    re-read the heartbeat word after a delay to show it advancing.
sleep 3
HB=$(printf "0x%X" $(( BEACON + 12 )))   # beacon[3] = heartbeat
cat > /tmp/hp-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 $BEACON, 0x10
Sleep 400
mem32 $HB, 0x4
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/hp-read.jlink 2>/tmp/hp-read.err > /tmp/hp-read.out || true
echo "----- $NAME M55-HP SRAM0 beacon (magic / CPUID / VTOR / heartbeat) -----"
grep -iE "^$(printf '%08X' $BEACON)| = " /tmp/hp-read.out | head
echo "(heartbeat re-read below should differ from beacon[3] above = HP actively running)"
echo "-----------------------------------------------------------------------"
