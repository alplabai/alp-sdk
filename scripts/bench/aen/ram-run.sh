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
	         Rebuild it with the RAM console layered on top:
	             scripts/bench/aen/build.sh <app-dir> \
	                 -DEXTRA_CONF_FILE=$ALP_SDK_DIR/scripts/bench/aen/aen-bench-shared.conf
	         (that fragment sets CONFIG_RAM_CONSOLE=y + CONFIG_UART_CONSOLE=n).
	         The app itself is unchanged -- its committed prj.conf keeps the
	         customer-facing UART console.
	EOF
	exit 3
fi
BUF=0x$BUF_SYM
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
  bench_jlink_mem8_chunks "$BUF" "$SIZE"
  echo qc
} > "$SCRIPT"
echo ">>> RAM-run $(basename "$BD")  entry=$ENTRY  ram_console_buf=$BUF  sleep=${SLEEP}ms" >&2
# shellcheck disable=SC2046  # word-splitting bench_jlink_select is intentional
# JLinkExe can emit "Cannot connect to the probe" on EITHER stream depending on
# where the failure happens; merge into one file (the awk decoder below only
# matches "<hex addr> = ..." lines, so the extra text is harmless) so the grep
# below actually sees it instead of missing a stderr-only failure.
$JLINK $(bench_jlink_select) -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript "$SCRIPT" > /tmp/jlink.out 2>&1 || true
# A probe that never connected looks exactly like an app that printed nothing;
# say which, instead of leaving the operator to diff two empty consoles.
if grep -qi "Cannot connect to the probe" /tmp/jlink.out; then
	echo "ram-run: J-Link probe not selected/reachable -- JLINK_SN='${JLINK_SN:-<unset>}'." >&2
	echo "         With more than one J-Link attached you MUST export JLINK_SN." >&2
fi
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
