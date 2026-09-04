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
# See ram-run.sh for why the selector is conditional on JLINK_SN.
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
# See ram-run.sh (issue #935): if BUF_SYM is empty, do NOT fold it into BUF --
# BUF would silently become the bare string "0x" and step 4's `mem8 $BUF,
# $SIZE` would run as `mem8 0x, $SIZE`, printing an EMPTY "RAM console" block
# indistinguishable from a boot failure. Step 4 below checks BUF_SYM directly.
BUF_SYM=$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
BUF=0x$BUF_SYM

# 0. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. Same DPIDR gate as
# flash-jlink-mramxip.sh (see that script for the full rationale): JLINK_SN
# narrows probe choice but does not itself prove which board answered. Hard
# ABORT, not a warning -- read-only connect first, no writes until confirmed.
cat > /tmp/flowd-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/flowd-preflight.jlink \
  > /tmp/flowd-preflight.out 2>&1 || true
bench_jlink_assert_aen_dpidr /tmp/flowd-preflight.out "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

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
echo ">>> FLOW-D J-Link flash $NAME  (ram_console_buf=${BUF_SYM:-none (UART console)})" >&2
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
# Write the transcript FIRST, fully, then grep|head it for display (#1488
# finding 5) -- a `... | tee out | grep ... | head -N` pipeline lets `head`
# exit after N lines and SIGPIPE grep, which then closes tee's stdout pipe;
# tee can die from that SIGPIPE before JLinkExe's full transcript (including
# the `Verify successful.` / `Verify failed.` line the gate below depends on)
# is written to disk. Once a genuinely good flash's transcript got truncated
# that way, the absence of "verify successful" in the truncated file would
# read as a hard exit 3 on a board that actually flashed fine.
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/flowd.jlink > /tmp/flowd.out 2>&1 || true
grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found" /tmp/flowd.out | head -30
echo "----- (full log: /tmp/flowd.out) -----"
if grep -qi "Could not connect to the target device" /tmp/flowd.out; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe (same blocker the doc records)."
  echo "   The MRAM was NOT written. Check the new probe's firmware / connect-under-reset behaviour."
  exit 2
fi

# GATE ON THE VERIFY RESULT (#1488) -- same defect flash-jlink-hp.sh was fixed
# for under #1343. The `verifybin` above was issued but its outcome was never
# read: the output went to a display-only pipe and the connect check was the
# only thing that could fail this script, so a `Verify failed.` exited 0 and
# reported a good flash.
if grep -qiE "verify failed|verification failed|mismatch" /tmp/flowd.out; then
  echo "!! VERIFY FAILED -- the bytes on the part do NOT match $PKG."
  grep -iE "verify failed|verification failed|mismatch" /tmp/flowd.out | head -5
  echo "   Do not treat this board as flashed."
  exit 3
fi
if ! grep -qi "verify successful" /tmp/flowd.out; then
  echo "!! no verifybin success reported -- treating as FAILED (the verify never ran)."
  exit 3
fi
echo "verify: verifybin OK ($PKG @ $ADDR)"

# 4. SES has re-booted the app; attach read-only with the GENERIC device and dump
#    the RAM console (the part-number profile can't re-halt the running secure core).
sleep 3
if [ -z "$BUF_SYM" ]; then
  echo "----- $NAME RAM console: no 'ram_console_buf' in this image (UART-console app) -----" >&2
  echo "      the flash above still completed -- this is not a boot failure. Read the" >&2
  echo "      console via the labgrid 'console' resource instead." >&2
else
  cat > /tmp/flowd-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
  "${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/flowd-read.jlink 2>/tmp/flowd-rd.err > /tmp/flowd-rd.out || true
  # JLinkExe exits 0 even when it never opened the probe, so `|| true` above
  # hides a total connect failure and the decode below would render it as
  # empty target output (alp-sdk#1318).
  bench_jlink_assert_connected /tmp/flowd-rd.out "Flow D read-back" || exit 7
  echo "----- $NAME RAM console (flow-D flashed, SE-booted) -----"
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/flowd-rd.out
  echo; echo "--------------------------------------------------------"
fi
