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
# See ram-run.sh for why the selector is conditional on JLINK_SN. The MRAM
# write itself happens inside flash-jlink.sh (invoked below), which now
# selects by the same JLINK_SN; this array is only for the read_console()
# probe here.
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")
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
  # SAFETY GATE (alp-sdk#813) -- confirm the AEN E8 answered BEFORE the mem8
  # read below. flash-jlink.sh (invoked earlier in the caller's loop) gates
  # its own MRAM write, but this is a SEPARATE JLinkExe session -- one of
  # this bench's probes shares OEM serial 603000869 with the GD32 bridge on
  # a DIFFERENT board, so this session needs its own DP ID proof too.
  cat > /tmp/rdc-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
  "${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/rdc-preflight.jlink \
    > /tmp/rdc-preflight.out 2>&1 || true
  bench_jlink_assert_connected /tmp/rdc-preflight.out "Flow D console read preflight" || exit 7
  bench_jlink_assert_aen_dpidr /tmp/rdc-preflight.out "Flow D console read preflight" || exit 4
  # bench_jlink_mem8_chunks: JLinkExe rejects a single mem8 read over 0x10000
  # and fails silently -- SIZE here (0xB00) is well under that, but chunk
  # anyway so this stays correct if SIZE ever grows.
  {
    echo "device $JLINK_DEVICE_READ"
    echo si SWD
    echo "speed $JLINK_SPEED"
    echo connect
    bench_jlink_mem8_chunks "$BUF" "$SIZE"
    echo exit
  } > /tmp/rdc.jlink
  "${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/rdc.jlink 2>/dev/null > /tmp/rdc.out || true
  # JLinkExe exits 0 even when it never opened the probe, so `|| true` above
  # hides a total connect failure and the decode below would render it as
  # empty target output (alp-sdk#1318).
  bench_jlink_assert_connected /tmp/rdc.out "Flow D console read" || exit 7
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/rdc.out
}

SUM=/tmp/flowd-batch-summary.txt; : > "$SUM"
for a in "${APPS[@]}"; do
  BD="$BENCH_ROOT/build/$a"
  echo "##################################################"
  echo "########## $a"
  echo "##################################################"
  if [ ! -f "$BD/zephyr/zephyr.bin" ]; then echo "SKIP: no zephyr.bin"; echo "$a : SKIP (no build)" >>"$SUM"; continue; fi
  # Flow D flash. `frc=0; ... || frc=$?` (NOT a bare `flog=$(...)`) so this
  # command substitution's exit status can never trip `set -e` (line 18):
  # flash-jlink.sh now hard-exits 3 on a failed/missing verifybin (#1488),
  # and under errexit a bare assignment would abort this whole strictly-
  # serial batch at the FIRST bad verify -- muting the very diagnostic that
  # was just captured into $flog (the echo below would never run) and never
  # reaching the "BATCH SUMMARY" cat at the bottom. Reset frc every iteration.
  frc=0
  flog=$(timeout 120 bash "$HERE/flash-jlink.sh" "$BD" "$SIZE" 2>&1) || frc=$?
  echo "$flog" | grep -iE "package:|Connecting to J-Link|Verify|FAILED|Could not connect|Programming flash" | head -6
  # The grep|head -6 above is a summary, and on a failure it is the WRONG six
  # lines: flash-jlink.sh displays up to 30 transcript lines of its own before
  # it ever reaches the verify gate, so the gate's terminal diagnostic
  # ("!! VERIFY FAILED ..." / "!! no verifybin success reported ...") is past
  # the head cut and the operator sees a bare FLASH-UNVERIFIED label with none
  # of the evidence. Dump the tail of the captured log whenever the child
  # failed, before the summary line below.
  if [ "$frc" -ne 0 ]; then
    echo "----- flash-jlink.sh tail (exit $frc) -----"
    printf '%s\n' "$flog" | tail -20
    echo "-------------------------------------------"
  fi
  case "$frc" in
  0) ;; # fall through to the post-flash console read below
  3)
    echo ">> $a : FLASH-UNVERIFIED"; echo "$a : FLASH-UNVERIFIED" >>"$SUM"; continue
    ;;
  2)
    echo ">> $a : FLASH-FAILED (probe/target connect)"; echo "$a : FLASH-FAILED" >>"$SUM"; continue
    ;;
  4)
    echo ">> $a : FLASH-ABORTED (wrong probe / DPIDR mismatch)"; echo "$a : FLASH-ABORTED (wrong probe)" >>"$SUM"; continue
    ;;
  7)
    echo ">> $a : FLASH-OK-READBACK-FAILED (flash+verify succeeded, post-boot console read did not)"
    echo "$a : FLASH-OK-READBACK-FAILED" >>"$SUM"; continue
    ;;
  *)
    echo ">> $a : FLASH-ERROR (exit $frc) -- see log above"; echo "$a : FLASH-ERROR (exit $frc)" >>"$SUM"; continue
    ;;
  esac
  # let slow apps finish (ethernet DHCP ~17s, NPU inference, PDM capture)
  sleep 16
  echo "----- RAM console ($a) -----"
  # Same mute-abort shape as the flash capture above, for the same reason:
  # read_console ends in `bench_jlink_assert_connected ... || exit 7`, and a
  # bare `con=$(read_console ...)` assignment takes that status, so under
  # errexit (line 18) a probe that never opened would abort the whole
  # strictly-serial batch here -- after the flash already succeeded, and
  # before the BATCH SUMMARY `cat "$SUM"` at the bottom ever ran. Capture the
  # status instead and log the app as CONSOLE-READ-FAILED.
  crc=0
  con=$(read_console "$BD") || crc=$?
  echo "$con"
  if [ "$crc" -ne 0 ]; then
    echo ">> $a : CONSOLE-READ-FAILED (exit $crc)"
    echo "$a : CONSOLE-READ-FAILED (exit $crc)" >>"$SUM"
    echo
    continue
  fi
  res=$(echo "$con" | grep -iE "RESULT" | tail -1)
  [ -z "$res" ] && res="(no RESULT line — see console above)"
  echo "$a : $res" >>"$SUM"
  echo
done
echo "##################################################"
echo "########## BATCH SUMMARY"
echo "##################################################"
cat "$SUM"
