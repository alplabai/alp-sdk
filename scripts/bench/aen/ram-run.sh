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
#   - the load address is DERIVED from the ELF's first LOAD segment p_paddr
#     (readelf -l), not hard-coded 0x0 -- an app that hard-codes
#     CONFIG_FLASH_LOAD_OFFSET (e.g. the slot0 offset 0x10000) still links
#     correctly at a non-zero ITCM address and must be loaded there. If the
#     derived base is >= 0x80000000 the image is slot0/MRAM-linked and Flow C
#     cannot run it (loading it at its resident address just re-enters the
#     already-resident MRAM image, not the freshly built one) -- refuse rather
#     than silently mis-run it.
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
BUF_SYM=$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
if [ -z "$BUF_SYM" ]; then
	# No RAM console linked in.  Without this guard BUF would be the bare
	# string "0x", JLink would run `mem8 0x, <size>`, and the operator would
	# get an EMPTY "RAM console (decoded)" block with no hint why -- which
	# reads as "the app crashed" when the app is fine and simply routed its
	# output to a UART.  Flow C produces no capturable UART output, so a
	# UART-console app is invisible here (issue #935).
	cat >&2 <<-EOF
	ram-run: '$ELF' has no 'ram_console_buf' symbol -- this app was built
	         with the UART console, which Flow C cannot capture.
	         Rebuild it with the RAM console AND the Flow C link-offset
	         override layered on top (both fragments, in this order):
	             scripts/bench/aen/build.sh <app-dir> \
	                 -DEXTRA_CONF_FILE="$ALP_SDK_DIR/scripts/bench/aen/aen-bench-shared.conf;$ALP_SDK_DIR/scripts/bench/aen/aen-flowc-itcm.conf"
	         (aen-bench-shared.conf sets CONFIG_RAM_CONSOLE=y +
	         CONFIG_UART_CONSOLE=n; aen-flowc-itcm.conf sets
	         CONFIG_USE_DT_CODE_PARTITION=n + CONFIG_FLASH_LOAD_OFFSET=0x0 --
	         Flow-C-only, do not use it for a Flow A/D MRAM build).
	         The app itself is unchanged -- its committed prj.conf keeps the
	         customer-facing UART console.
	EOF
	exit 3
fi
BUF=0x$BUF_SYM

BASE_RAW=$($OBJ-readelf -l "$ELF" | awk '/^[[:space:]]*LOAD[[:space:]]/{print $4; exit}')
if [ -z "$BASE_RAW" ]; then
	echo "ram-run: could not find a LOAD segment in '$ELF' -- can't derive the load address." >&2
	exit 4
fi
BASE=$(printf '0x%X' "$BASE_RAW")
if (( BASE_RAW >= 0x80000000 )); then
	cat >&2 <<-EOF
	ram-run: '$ELF' is slot0/MRAM-linked (first LOAD segment at $BASE) --
	         Flow C cannot RAM-run this: loading it at its resident address
	         just re-enters the ALREADY-RESIDENT MRAM image, not the freshly
	         built one, and JLinkExe will not warn you. Rebuild with the
	         ITCM retarget (see docs/aen-bench-bringup.md, Flow C) or use
	         Flow D (scripts/bench/aen/flash-jlink-mramxip.sh).
	EOF
	exit 5
fi

SCRIPT=$(mktemp /tmp/jlink.XXXX.jlink)
{
  echo connect
  echo halt
  [ -n "$PRELOAD" ] && cat "$PRELOAD"
  echo "loadbin $BIN $BASE"
  echo "setpc $ENTRY"
  echo go
  echo "Sleep $SLEEP"
  echo halt
  echo "mem8 $BUF, $SIZE"
  echo qc
} > "$SCRIPT"
echo ">>> RAM-run $(basename "$BD")  entry=$ENTRY  base=$BASE  ram_console_buf=$BUF  sleep=${SLEEP}ms" >&2
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
