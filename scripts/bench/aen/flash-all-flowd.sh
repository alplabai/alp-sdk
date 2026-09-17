#!/usr/bin/env bash
# scripts/bench/aen/flash-all-flowd.sh [--atoc-unqueryable] [app-name ...]
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
# --atoc-unqueryable is forwarded verbatim to every flash-jlink.sh call, for
# a bench slot with no SE-UART wired (e.g. e1m-aen-evk-03, where Flow D is
# the ONLY load path). It is opt-in here for the same reason it is opt-in
# there: it acknowledges that the resident-ATOC check did not run, and the
# whole point of #2029/#2027 is that a human says that once, deliberately.
# Hardcoding it at the call site below would make the batch replace the ATOC
# blindly on every run with the acknowledgement nowhere -- re-entering the
# #2025 hazard through the batch runner. --replace-atoc is deliberately NOT
# forwarded: see flash-jlink.sh's header on why the two must never merge.
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export
# SETOOLS_DIR before running. See README.md.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

OBJNM="$(bench_tool_prefix)-nm" || exit $?
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns. The MRAM write
# itself happens inside flash-jlink.sh (invoked below), which now routes the
# same way; this array is only for the read_console() probe here.
JLINK_ARGS=(bench_jlink_run)
SIZE=0xB00

# Flag scan (alp-sdk#2189). Deliberately the same whole-argv `for` shape
# flash-jlink.sh uses for this exact flag, NOT flash-run.sh's while/shift
# loop: this script's positionals are a variable-length app list, so a parser
# that stops honouring flags at the first non-option token would silently
# ignore `flash-all-flowd.sh aen-wdt-feed --atoc-unqueryable` -- and silently
# ignoring THIS flag means every entry aborts with exit 8 again, which is the
# bug being fixed. Both Flow D flags keep one parsing convention across the
# two scripts; see flash-jlink.sh's PARSER SHAPE note before changing either.
ATOC_UNQUERYABLE=()
POSITIONAL=()
for arg in "$@"; do
	case "$arg" in
	--atoc-unqueryable) ATOC_UNQUERYABLE=(--atoc-unqueryable) ;;
	*) POSITIONAL+=("$arg") ;;
	esac
done
set -- "${POSITIONAL[@]}"

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
  cat > /tmp/rdc.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
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
  flog=$(timeout 120 bash "$HERE/flash-jlink.sh" "${ATOC_UNQUERYABLE[@]}" "$BD" "$SIZE" 2>&1) || frc=$?
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
  8)
    # bench_flowd_atoc_guard's refusal: no SE_UART and no --atoc-unqueryable
    # (bench-env.sh). Unlike every other arm here this is a BATCH-level
    # configuration refusal, not a per-app failure -- it is decided before
    # any probe or target access, so it cannot differ between apps and every
    # remaining entry would abort identically. Breaking out says so once
    # instead of printing the same guard text N times and handing back a
    # summary that is 100% FLASH-REFUSED with no board ever written
    # (alp-sdk#2189). The already-processed entries keep their real verdicts
    # in $SUM, and the BATCH SUMMARY below still prints.
    echo ">> $a : FLASH-REFUSED (no resident-ATOC check available)"
    echo "$a : FLASH-REFUSED (no resident-ATOC check)" >>"$SUM"
    echo "!! ABORTING BATCH: flash-jlink.sh refuses to write without a resident-ATOC check."
    echo "   This is a run-level setting, so every remaining app would refuse identically."
    echo "   Either export SE_UART for a slot that has one wired, or re-run this script"
    echo "   with --atoc-unqueryable to acknowledge this slot has no SE-UART."
    break
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
