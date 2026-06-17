#!/usr/bin/env bash
# scripts/bench/aen/flash-all-flowd.sh [app-name ...]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives flash-jlink.sh = JLinkExe + the Alif SETOOLS). Runs under WSL2
# on Windows. See docs/aen-bench-bringup.md.
#
# Flash each AEN bench app over FLOW D (J-Link direct MRAM), boot it, and
# capture its RESULT line. Strictly serial (one board / one probe).
# Resilient: a failed app is logged and the batch continues.
#
# App list: the names given on argv, else the committed apps.txt (one
# build-dir name per line, '#' comments ignored). Each name is a
# directory under $BENCH_ROOT/build/.
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export
# SETOOLS_DIR before running. See README.md.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

OBJNM="$(bench_tool_prefix)-nm" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
SIZE=0xB00

# App list: argv wins; otherwise read apps.txt (prefer the committed list).
if [ "$#" -gt 0 ]; then
	APPS=("$@")
else
	APPS=()
	while IFS= read -r line; do
		line="${line%%#*}"
		line="$(echo "$line" | xargs)"   # trim
		[ -n "$line" ] && APPS+=("$line")
	done < "$HERE/apps.txt"
fi

# Non-halting RAM-console read (generic device; does NOT leave the core halted).
read_console() {
  local BD="$1"
  local BUF; BUF=0x$($OBJNM "$BD/zephyr/zephyr.elf" 2>/dev/null | awk '/ ram_console_buf$/{print $1}')
  [ "$BUF" = "0x" ] && { echo "(no ram_console_buf in elf)"; return; }
  cat > /tmp/rdc.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
  $JLINK -nogui 1 -CommanderScript /tmp/rdc.jlink 2>/dev/null > /tmp/rdc.out || true
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/rdc.out
}

SUM=/tmp/flowd-batch-summary.txt; : > "$SUM"
for a in "${APPS[@]}"; do
  BD="$BENCH_ROOT/build/$a"
  echo "##################################################"
  echo "########## $a"
  echo "##################################################"
  if [ ! -f "$BD/zephyr/zephyr.bin" ]; then echo "SKIP: no zephyr.bin"; echo "$a : SKIP (no build)" >>"$SUM"; continue; fi
  # Flow D flash (capture connect/verify status only)
  flog=$(timeout 120 bash "$HERE/flash-jlink.sh" "$BD" "$SIZE" 2>&1)
  echo "$flog" | grep -iE "package:|Connecting to J-Link|Verify|FAILED|Could not connect|Programming flash" | head -6
  if echo "$flog" | grep -qiE "Could not connect to the target device|Cannot connect to the probe"; then
    echo ">> $a : FLASH-FAILED (probe/target connect)"; echo "$a : FLASH-FAILED" >>"$SUM"; continue
  fi
  if ! echo "$flog" | grep -qi "Verify successful"; then
    echo ">> $a : FLASH-UNVERIFIED"; echo "$a : FLASH-UNVERIFIED" >>"$SUM"; continue
  fi
  # let slow apps finish (ethernet DHCP ~17s, NPU inference, PDM capture)
  sleep 16
  echo "----- RAM console ($a) -----"
  con=$(read_console "$BD")
  echo "$con"
  res=$(echo "$con" | grep -iE "RESULT" | tail -1)
  [ -z "$res" ] && res="(no RESULT line — see console above)"
  echo "$a : $res" >>"$SUM"
  echo
done
echo "##################################################"
echo "########## BATCH SUMMARY"
echo "##################################################"
cat "$SUM"
