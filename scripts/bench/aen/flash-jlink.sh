#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink.sh <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe + the Alif SETOOLS, both Linux binaries on this
# bench). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- J-Link DIRECT MRAM flash (no SE-UART), per docs/aen-bench-bringup.md.
#
# Writes the SAME signed ATOC package SETOOLS (flow A) burns, to the SAME MRAM
# address, but over SWD via J-Link's built-in Alif MRAM loader -- which only
# activates for the part-number device profile $JLINK_DEVICE_FLASH (NOT the
# generic Cortex-M55 used for reads/RAM-run). Then a PIN reset (RSetType 2) re-runs
# the SE boot ROM, which verifies + boots the image from MRAM exactly as on a
# SETOOLS flash. No re-signing, no keys, no SE-UART maintenance-window race.
#
# This bench's examples are ITCM-load-via-ATOC (a single self-contained
# AppTocPackage.bin written as ONE blob at the package start address; the SE loads
# the embedded app to ITCM 0x58000000 and boots), so this writes that one blob --
# NOT the slot0-XIP two-blob (app@0x80010000 + ATOC) variant in the doc.
#
# Prereqs: new probe running Alif-aware firmware so `device $JLINK_DEVICE_FLASH`
# CONNECTS (the old J-Link PLUS fw 2023-01-30 failed this connect-under-reset -- the
# whole reason flow A was used here). J-Link V9.46+ has the MRAM loader built in.
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SIZE="${2:-0x500}"
bench_require_setools || exit $?
SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
BUF=0x$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')

# 1. stage the image + the per-app signed-ATOC config (same JSON flow-run.sh uses)
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON

cd "$SET"
echo ">>> FLOW-D J-Link flash $NAME  (ram_console_buf=$BUF)" >&2
# 2. build the signed ATOC package (app-gen-toc only -- NO SE-UART) + read its
#    MRAM placement from the generated map (shifts per build/config -- never hardcode).
./app-gen-toc -f "build/config/$NAME.json" >/tmp/gentoc.log 2>&1 || { echo "gen-toc FAILED"; tail /tmp/gentoc.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ADDR" ] && { echo "could not parse APP Package Start Address from build/app-package-map.txt"; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ADDR" >&2

# 3. J-Link CommanderScript: part-number device unlocks the MRAM loader; write +
#    verify the package, then PIN reset (RSetType 2) so the SE boot ROM reloads it.
cat > /tmp/flowd.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $DEV
connect
loadbin $PKG $ADDR
verifybin $PKG $ADDR
RSetType 2
r
g
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd.jlink 2>&1 | tee /tmp/flowd.out | \
  grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found" | head -30
echo "----- (full log: /tmp/flowd.out) -----"
if grep -qi "Could not connect to the target device" /tmp/flowd.out; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe (same blocker the doc records)."
  echo "   The MRAM was NOT written. Check the new probe's firmware / connect-under-reset behaviour."
  exit 2
fi

# 4. SES has re-booted the app; attach read-only with the GENERIC device and dump
#    the RAM console (the part-number profile can't re-halt the running secure core).
sleep 3
cat > /tmp/flowd-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-read.jlink 2>/tmp/flowd-rd.err > /tmp/flowd-rd.out || true
echo "----- $NAME RAM console (flow-D flashed, SE-booted) -----"
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/flowd-rd.out
echo; echo "--------------------------------------------------------"
