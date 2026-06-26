#!/usr/bin/env bash
# scripts/bench/aen/ram-run.sh <build-dir> [sleep_ms] [bufsize_hex] [preload_jlink_file]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe). Runs under WSL2 on Windows. No SETOOLS/SE-UART —
# this flow never writes MRAM. See docs/aen-bench-bringup.md.
#
# FLOW C -- RAM-run a Zephyr ITCM image on the E8 (M55-HE) over J-Link and
# ASCII-decode the CONFIG_RAM_CONSOLE buffer ('ram_console_buf') read back over SWD.
#   - loadbin does an implicit SYSRESETREQ + halt-at-reset-vector; we then
#     setpc <entry> + go (loadbin alone does NOT reliably enter our vectors).
#   - optional preload file: extra JLink commands run AFTER halt, BEFORE loadbin
#     (e.g. clear a SoC integration reg for the cold-RAM-run gotcha).
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SLEEP="${2:-1500}"
SIZE="${3:-0x600}"
PRELOAD="${4:-}"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
ELF="$BD/zephyr/zephyr.elf"
BIN="$BD/zephyr/zephyr.bin"
ENTRY_RAW=$($OBJ-readelf -h "$ELF" | awk '/Entry point/{print $NF}')
ENTRY=$(printf '0x%X' $(( ENTRY_RAW & ~1 )))         # clear thumb bit
BUF=0x$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
SCRIPT=$(mktemp /tmp/jlink.XXXX.jlink)
{
  echo connect
  echo halt
  [ -n "$PRELOAD" ] && cat "$PRELOAD"
  echo "loadbin $BIN 0x0"
  echo "setpc $ENTRY"
  echo go
  echo "Sleep $SLEEP"
  echo halt
  echo "mem8 $BUF, $SIZE"
  echo qc
} > "$SCRIPT"
echo ">>> RAM-run $(basename "$BD")  entry=$ENTRY  ram_console_buf=$BUF  sleep=${SLEEP}ms" >&2
$JLINK -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript "$SCRIPT" 2>/tmp/jlink.err > /tmp/jlink.out || true
echo "----- RAM console (decoded) -----"
# Decode the 'ADDR = HH HH ...' mem8 lines into ASCII; stop at first NUL run.
awk '
/^[0-9A-Fa-f]+ = / {
  for (i=3; i<=NF; i++) {
    if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue
    b = strtonum("0x" $i)
    if (b == 0) { nul++; if (nul > 4) exit; next }
    nul = 0
    if (b == 10 || b == 13) { printf "\n"; continue }
    if (b >= 32 && b < 127) printf "%c", b
  }
}' /tmp/jlink.out
echo
echo "---------------------------------"
rm -f "$SCRIPT"
