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
BUF=0x$($OBJ-nm "$BD/zephyr/zephyr.elf" | awk '/ ram_console_buf$/{print $1}')
cat > /tmp/rr.jlink <<EOF
connect
halt
mem8 $BUF, $SIZE
qc
EOF
$JLINK -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript /tmp/rr.jlink 2>/dev/null > /tmp/rr.out || true
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/rr.out
echo; echo "(buf=$BUF)"
