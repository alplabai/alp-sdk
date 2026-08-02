#!/usr/bin/env bash
# scripts/bench/aen/reread.sh <build-dir> [size]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# Re-read ram_console_buf over SWD (no reflash) — attach the GENERIC
# Cortex-M55 device, halt, dump the RAM console, and ASCII-decode it.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SIZE="${2:-0x500}"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
# See ram-run.sh (issue #935): if BUF_SYM is empty, do NOT fold it into BUF --
# BUF would silently become the bare string "0x" and `mem8 $BUF, $SIZE` would
# run as `mem8 0x, $SIZE`, printing an EMPTY "RAM console" block
# indistinguishable from a boot failure.
BUF_SYM=$($OBJ-nm "$BD/zephyr/zephyr.elf" | awk '/ ram_console_buf$/{print $1}')
if [ -z "$BUF_SYM" ]; then
  echo "reread: no 'ram_console_buf' in this image (UART-console app) -- read the console via the labgrid 'console' resource instead." >&2
  exit 3
fi
BUF=0x$BUF_SYM
{
	echo connect
	echo halt
	bench_jlink_mem8_chunks "$BUF" "$SIZE"
	echo qc
} > /tmp/rr.jlink
# shellcheck disable=SC2046  # word-splitting bench_jlink_select is intentional
# "Cannot connect to the probe" can land on either stream -- stderr used to be
# discarded (2>/dev/null), so a stderr-only failure never tripped the grep
# below. Merge it in; the awk decoder only matches "<hex addr> = ..." lines so
# the extra text is harmless.
$JLINK $(bench_jlink_select) -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript /tmp/rr.jlink > /tmp/rr.out 2>&1 || true
if grep -qi "Cannot connect to the probe" /tmp/rr.out; then
	echo "reread: J-Link probe not selected/reachable -- export JLINK_SN (multi-probe host)." >&2
fi
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/rr.out
echo; echo "(buf=$BUF)"
