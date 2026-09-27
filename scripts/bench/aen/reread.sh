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
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns.
JLINK_ARGS=(bench_jlink_run)
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
# SAFETY GATE (alp-sdk#813) -- confirm the AEN E8 answered BEFORE the
# halt+mem8 read below. This bench has two probes sharing OEM serial
# 603000869, one of them on the GD32 bridge on a DIFFERENT board (V2N-M1);
# JLinkExe selects by serial only, so JLINK_SN alone cannot prove which
# board is on the other end -- see bench-env.sh. Read-only connect first;
# nothing is halted until the DP ID matches.
cat > /tmp/reread-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/reread-preflight.jlink \
  > /tmp/reread-preflight.out 2>&1 || true
bench_jlink_assert_connected /tmp/reread-preflight.out "re-read preflight" || exit 7
bench_jlink_assert_aen_dpidr /tmp/reread-preflight.out "re-read preflight" || exit 4

# mem8 is chunked at 0x10000 bytes (JLinkExe rejects a single NumBytes above
# that, alp-sdk#2313) via the shared bench_mem8_chunks() helper -- see its
# header comment in bench-env.sh. The decoder below is unaffected: it scans
# every matching 'ADDR = HH HH ...' line in file order, which is address order.
MEM8_LINES="$(bench_mem8_chunks "$BUF" "$SIZE")" || exit $?
{
	echo connect
	echo halt
	printf '%s\n' "$MEM8_LINES"
	echo qc
} > /tmp/rr.jlink
"${JLINK_ARGS[@]}" -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript /tmp/rr.jlink 2>/dev/null > /tmp/rr.out || true
# JLinkExe exits 0 even when it never opened the probe, so `|| true` above
# hides a total connect failure and the decode below would render it as
# empty target output (alp-sdk#1318).
bench_jlink_assert_connected /tmp/rr.out "re-read" || exit 7
# A CONNECTED session can still fail the READ itself (an explicit "Could not
# read memory.", a whole-transcript-empty read, or one chunk's dump line
# missing while the others come back) -- see bench_mem8_verify_chunks()'s
# header comment in bench-env.sh (alp-sdk#2313).
bench_mem8_verify_chunks "reread" /tmp/rr.out "$MEM8_LINES" || exit $?
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/rr.out
echo; echo "(buf=$BUF)"
