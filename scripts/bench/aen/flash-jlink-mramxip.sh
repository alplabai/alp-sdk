#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-mramxip.sh <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe + the Alif SETOOLS, both Linux binaries on this
# bench). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- MRAM-XIP / slot0 TWO-BLOB variant (per docs/aen-bench-bringup.md §Flow D).
#
# NOT the only way to provision this shape: bench-proven 2026-07-19, a plain
# `west flash` / `app-write-mram -p` over the SE-UART (Flow A, the `alif_flash`
# runner) burns the same two blobs in one pass -- the runner auto-detects the
# shape from the app's own reset vector. This script is the faster SWD-only
# alternative (no SE-UART reset race), not a capability requirement.
#
# Unlike flash-jlink.sh (single-blob ITCM-load-via-ATOC, loadAddress 0x58000000),
# this is for an app LINKED INTO MRAM slot0 (@0x80010000, CONFIG_FLASH_LOAD_OFFSET
# 0x10000) that overflows ITCM -- e.g. a real NPU model. Two blobs are written:
#   1. zephyr.bin        -> 0x80010000  (the slot0-linked app)
#   2. AppTocPackage.bin -> <parsed>    (the signed ATOC the SE boots)
# The app entry uses mramAddress 0x80010000 (the FULL address -- the 0x10000
# OFFSET gives SETOOLS "Invalid Global Address") + flags ["boot"] (NOT loadAddress),
# so app-gen-toc signs the app where it sits in MRAM rather than embedding it.
#
# The app build MUST set CONFIG_USE_DT_CODE_PARTITION=y so it links at the slot0
# offset (reset vector 0x8001xxxx). Without it FLASH_LOAD_OFFSET stays 0 and the
# image links at the MRAM base (0x8000xxxx) and faults on an SE slot0 boot.
#
# GOTCHA -- returning to ITCM apps: once a slot0 image is resident, the SE boots
# it preferentially over an ITCM-load ATOC, and a J-Link `erase` does NOT clear
# MRAM. To flash ITCM-load (flow-C/flash-jlink.sh) apps afterwards, erase slot0
# first over the SE-UART:  app-write-mram -c $SE_UART -e "0x80010000 0x60000".
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SIZE="${2:-0x800}"
bench_require_setools || exit $?
SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
# Select the AEN J-Link by serial: the bench has TWO J-Links (AEN + the CC3501E
# XDS110/V2N), so without SelectEmuBySN JLinkExe picks arbitrarily and "Cannot
# connect to the probe". NO hardcoded serial default here: a bench-wide serial
# (e.g. 603000869) is SHARED by two probes that differ only by USB path, and a
# silent default can pick the WRONG board (the V2N-M1 GD32, not the AEN E8).
# Export JLINK_SN yourself if you need to disambiguate by serial -- either way,
# the DPIDR gate below (step 0b), not the serial, is what stops a write to the
# wrong target.
SEL="${JLINK_SN:+SelectEmuBySN $JLINK_SN}"
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
APP_ADDR=0x80010000                 # MRAM base 0x80000000 + slot0 offset 0x10000
BUF=0x$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')

# 0. SANITY: the image MUST be slot0-linked (reset-vector word reads 0x8001xxxx).
RV=$(xxd -e -l 8 "$BIN" | awk '{print $3}')   # 2nd LE word = reset vector
echo ">>> FLOW-D MRAM-XIP $NAME  (reset vector=0x$RV  ram_console_buf=$BUF)" >&2
case "$RV" in
  8001*) : ;;  # good -- linked into slot0 (0x80010000 + reset-handler offset)
  8000*) echo "!! reset vector 0x$RV is BASE-linked (0x8000xxxx), not slot0."
         echo "   Add CONFIG_USE_DT_CODE_PARTITION=y so FLASH_LOAD_OFFSET=0x10000."
         exit 3 ;;
  *) echo "!! reset vector 0x$RV unexpected -- not a 0x8001xxxx slot0 image."
     echo "   Drop any &itcm overlay; let the board default link into MRAM slot0."
     exit 3 ;;
esac

# 0b. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. The AEN E8 SW-DP IDR is 0x4C013477
# (BENCH-VERIFIED, see docs/bring-up-aen.md); the V2N-M1 GD32 probe reads
# 0x0BE12477. Flashing the wrong board is the one unrecoverable bench mistake,
# so this is a hard ABORT, not a warning -- read-only connect first, no writes
# happen until the ID is confirmed.
AEN_DPIDR="4C013477"
GD32_DPIDR="0BE12477"
cat > /tmp/flowd-mramxip-preflight.jlink <<EOF
$SEL
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip-preflight.jlink \
  > /tmp/flowd-mramxip-preflight.out 2>&1 || true
if grep -qi "$GD32_DPIDR" /tmp/flowd-mramxip-preflight.out; then
  echo "!! ABORT: probe reports SW-DP IDR 0x$GD32_DPIDR -- that is the V2N-M1" >&2
  echo "   GD32, NOT the AEN E8. Wrong probe selected (JLINK_SN='$JLINK_SN')." >&2
  echo "   Refusing to write MRAM. See /tmp/flowd-mramxip-preflight.out." >&2
  exit 4
fi
if ! grep -qi "$AEN_DPIDR" /tmp/flowd-mramxip-preflight.out; then
  echo "!! ABORT: expected AEN E8 SW-DP IDR 0x$AEN_DPIDR not seen on connect." >&2
  echo "   Refusing to write MRAM -- check JLINK_SN / wiring / probe selection." >&2
  cat /tmp/flowd-mramxip-preflight.out >&2
  exit 4
fi
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# 1. stage the app + write the slot0 (mramAddress) signed-ATOC config.
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME-slot0.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"] }
}
JSON

cd "$SET"
# 2. build the signed ATOC (app-gen-toc only) + read the ATOC MRAM placement.
./app-gen-toc -f "build/config/$NAME-slot0.json" >/tmp/gentoc-mramxip.log 2>&1 \
  || { echo "gen-toc FAILED"; tail -20 /tmp/gentoc-mramxip.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }
echo "    app  -> $APP_ADDR ($(stat -c%s "$SET/build/images/$NAME.bin") B)" >&2
echo "    atoc -> $ATOC_ADDR ($(stat -c%s "$PKG") B)" >&2

# 3. J-Link: part-number device unlocks the MRAM loader; write BOTH blobs, verify,
#    sanity-check the reset vector, then PIN reset (RSetType 2) -> SE boot ROM boots it.
cat > /tmp/flowd-mramxip.jlink <<EOF
$SEL
si SWD
speed $JLINK_SPEED
device $DEV
connect
loadbin $SET/build/images/$NAME.bin $APP_ADDR
loadbin $PKG $ATOC_ADDR
verifybin $SET/build/images/$NAME.bin $APP_ADDR
verifybin $PKG $ATOC_ADDR
mem32 $APP_ADDR 2
RSetType 2
r
g
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip.jlink 2>&1 | tee /tmp/flowd-mramxip.out | \
  grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found|= " | head -40
echo "----- (full log: /tmp/flowd-mramxip.out) -----"
if grep -qi "Could not connect to the target device" /tmp/flowd-mramxip.out; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe."; exit 2
fi

# 4. SES has re-booted the app; attach read-only (generic device) + dump RAM console.
sleep 3
cat > /tmp/flowd-mramxip-read.jlink <<EOF
$SEL
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip-read.jlink 2>/tmp/flowd-mramxip-rd.err > /tmp/flowd-mramxip-rd.out || true
echo "----- $NAME RAM console (flow-D MRAM-XIP flashed, SE-booted) -----"
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/flowd-mramxip-rd.out
echo; echo "--------------------------------------------------------"
