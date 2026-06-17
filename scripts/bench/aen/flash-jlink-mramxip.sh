#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-mramxip.sh <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe + the Alif SETOOLS, both Linux binaries on this
# bench). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- MRAM-XIP / slot0 TWO-BLOB variant (per docs/aen-bench-bringup.md §Flow D).
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
