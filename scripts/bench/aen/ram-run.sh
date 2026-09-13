#!/usr/bin/env bash
# scripts/bench/aen/ram-run.sh <build-dir> [sleep_ms] [bufsize_hex] [preload_jlink_file]
#
# Requires BENCH_PLACE (the labgrid-client place name whose probe to use, e.g.
# "e1m-aen-evk-02" -- you must already hold that place's reservation) and,
# unless the board-farm wrapper lives at the default $HOME/board-farm
# location, AEN_JLINK_RUN. See the BENCH_PLACE/AEN_JLINK_RUN comments below.
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe via the board-farm's jlink-run.sh wrapper). Runs under
# WSL2 on Windows. No SETOOLS/SE-UART -- this flow never writes MRAM. See
# docs/aen-bench-bringup.md.
#
# FLOW C -- RAM-run a Zephyr ITCM image on the E8 (M55-HE) over J-Link and
# ASCII-decode the CONFIG_RAM_CONSOLE buffer ('ram_console_buf') read back over SWD.
#   - loadbin does an implicit SYSRESETREQ + halt-at-reset-vector; we then
#     setpc <entry> + go (loadbin alone does NOT reliably enter our vectors).
#   - optional preload file: extra JLink commands run AFTER halt, BEFORE loadbin
#     (e.g. clear a SoC integration reg for the cold-RAM-run gotcha).
#   - the load address is DERIVED from the LOAD segment with the LOWEST
#     p_paddr among those with a NONZERO p_filesz (readelf -l), not hard-coded
#     0x0 and NOT just "the first LOAD segment" -- a Flow-C-ITCM build's first
#     LOAD segment is often a zero-FileSiz .bss segment in DTCM
#     (e.g. 0x20000228), and loading the image there splats it over live RAM.
#     An app that hard-codes CONFIG_FLASH_LOAD_OFFSET (e.g. the slot0 offset
#     0x10000) still links correctly at a non-zero ITCM address and must be
#     loaded there. If the derived base is >= 0x80000000 the image is
#     slot0/MRAM-linked and Flow C cannot run it (loading it at its resident
#     address just re-enters the already-resident MRAM image, not the freshly
#     built one) -- refuse rather than silently mis-run it. A second,
#     positive check also refuses any derived base that isn't 0x0, the ITCM
#     global alias (0x50000000 / 0x58000000), or SRAM (0x02xxxxxx) -- a DTCM
#     address slipping through here is exactly the bug this guards against.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SLEEP="${2:-1500}"
SIZE="${3:-0x600}"
PRELOAD="${4:-}"

# sleep_ms must be a plain non-negative decimal integer, checked BEFORE any
# probe access. `0x10`/`abc` silently became 0.000s through the old
# ms->seconds conversion, and a negative value (`-5`) only surfaced from
# `sleep`'s own error AFTER session 1 had already loaded+gone (alp-sdk#2076
# review).
if ! [[ "$SLEEP" =~ ^[0-9]+$ ]]; then
	echo "ram-run: sleep_ms '$SLEEP' is not a non-negative decimal integer -- refusing to guess." >&2
	exit 2
fi

# BENCH_PLACE -- the labgrid-client place name whose probe this run uses, routed
# through the board-farm's jlink-run.sh wrapper (alp-sdk#2076 review,
# alp-sdk#2064) instead of a bare -SelectEmuBySN. Five probes on this bench
# share OEM serial 000603000869; JLINK_SN alone cannot tell them apart, and
# the DPIDR preflight below only checks WHICH board answered THAT session --
# it does not stop session 1 loading one board while session 2 reads a
# DIFFERENT one under the same shared serial. jlink-run.sh instead resolves
# the named place's probe from labgrid and USB-masks every other probe so
# only it can be opened, closing that gap structurally rather than by
# checking after the fact. No default -- host/bench-specific, like
# SE_UART/AEN_OPENOCD_CFG in bench-env.sh (NOT edited here: PR #2080 and
# draft #2033 both touch that file).
BENCH_PLACE="${BENCH_PLACE:-}"
if [ -z "$BENCH_PLACE" ]; then
	echo "ram-run: BENCH_PLACE is unset. This bench has five J-Links sharing" >&2
	echo "         OEM serial 000603000869 -- a bare -SelectEmuBySN cannot tell" >&2
	echo "         them apart, so ram-run.sh requires the labgrid-client PLACE NAME" >&2
	echo "         whose probe to use, e.g.:" >&2
	echo "             export BENCH_PLACE=e1m-aen-evk-02" >&2
	echo "         (you must already hold that place's labgrid reservation)." >&2
	exit 2
fi

# AEN_JLINK_RUN -- the board-farm's per-probe isolation wrapper (labgrid
# reservation check + USB-namespace masking + firmware-safe JLinkExe
# selection). Defaults under $HOME, never hardcoded to one operator's home
# directory (scripts/check_public_private.py enforces this -- same
# discipline as bench_jlink_exe()'s $HOME/segger-latest default in
# bench-env.sh). Override to point at a fake sharing the wrapper's own
# "<place> [JLinkExe args...]" contract, e.g. in a test.
AEN_JLINK_RUN="${AEN_JLINK_RUN:-$HOME/board-farm/bin/jlink-run.sh}"
if [ ! -x "$AEN_JLINK_RUN" ]; then
	echo "ram-run: '$AEN_JLINK_RUN' not found or not executable -- install the" >&2
	echo "         board-farm tooling or export AEN_JLINK_RUN to point at it." >&2
	exit 2
fi

# jlink_run <JLinkExe args...> -- one isolated JLinkExe session against
# BENCH_PLACE's probe. Kept as one small function (rather than inlining the
# wrapper call at each site) so a test can override AEN_JLINK_RUN with a
# fake and exercise every call site through it, without driving the real
# wrapper's labgrid reservation check / USB masking / OpenOCD firmware read.
#
# The wrapper only special-cases `-CommandFile <path>` (it injects `exec
# DisableAutoUpdateFW` ahead of it, so a probe whose bundled firmware
# differs from the DLL is never offered an update) -- NOT `-CommanderScript`,
# which every other bench script in this repo uses. Both are the same
# JLinkExe option (confirmed against the real JLinkExe binary on this host:
# `-CommandFile` and `-CommanderScript` produce identical transcripts), but
# only `-CommandFile` is what the wrapper recognizes; passing
# `-CommanderScript` here would make it refuse (no file to inject into).
jlink_run() {
	"$AEN_JLINK_RUN" "$BENCH_PLACE" "$@"
}

# WORKDIR -- every JLinkExe transcript and generated CommandFile for this run
# lives under one mktemp -d directory, not fixed /tmp names. Fixed names
# (the old /tmp/jlink.out, /tmp/ram-run-preflight.out) let one run's
# transcript be overwritten by a CONCURRENT run on this same host moments
# before it is decoded -- reproduced running this script's own test suite
# against a real bench session: a pytest run's fake transcript was still
# sitting at /tmp/jlink-read.out after the test process exited. `trap ...
# EXIT` cleans it up on every exit path, including a `set -e` abort (e.g. a
# missing preload file) that used to leave a stray jlink-load.XXXX.jlink
# behind.
WORKDIR=$(mktemp -d /tmp/ram-run.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

OBJ="$(bench_tool_prefix)" || exit $?
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
	         Rebuild it with the RAM console AND the Flow C ITCM retarget
	         layered on top (both the conf fragments AND the overlay, in
	         this order):
	             scripts/bench/aen/build.sh <app-dir> \
	                 -DEXTRA_CONF_FILE="$ALP_SDK_DIR/scripts/bench/aen/aen-bench-shared.conf;$ALP_SDK_DIR/scripts/bench/aen/aen-flowc-itcm.conf" \
	                 -DEXTRA_DTC_OVERLAY_FILE="$ALP_SDK_DIR/scripts/bench/aen/aen-flowc-itcm.overlay"
	         (aen-bench-shared.conf sets CONFIG_RAM_CONSOLE=y +
	         CONFIG_UART_CONSOLE=n; aen-flowc-itcm.conf sets
	         CONFIG_USE_DT_CODE_PARTITION=n + CONFIG_FLASH_LOAD_OFFSET=0x0;
	         aen-flowc-itcm.overlay retargets zephyr,flash to &itcm and
	         drops zephyr,code-partition -- all Flow-C-only, do not use any
	         of them for a Flow A/D MRAM build. Both the conf half AND the
	         overlay half are required -- the conf alone still links into
	         MRAM).
	         The app itself is unchanged -- its committed prj.conf keeps the
	         customer-facing UART console.
	EOF
	exit 3
fi
BUF=0x$BUF_SYM

# Select the LOAD segment that objcopy -O binary actually emits from: the
# one with the LOWEST p_paddr among segments with a NONZERO p_filesz. Do NOT
# rely on segment order -- a DTCM .bss LOAD segment (p_filesz=0) commonly
# sorts first in an ITCM-retargeted link.
BASE_RAW=$($OBJ-readelf -l "$ELF" | awk '
/^[[:space:]]*LOAD[[:space:]]/ {
	paddr_hex = $4
	if (strtonum($5) != 0 && (!found || strtonum($4) < min)) {
		min = strtonum($4); min_hex = paddr_hex; found = 1
	}
}
END { if (found) print min_hex }
')
if [ -z "$BASE_RAW" ]; then
	echo "ram-run: could not find a LOAD segment with nonzero FileSiz in '$ELF' -- can't derive the load address." >&2
	exit 4
fi
if ! [[ "$BASE_RAW" =~ ^0x[0-9A-Fa-f]+$ ]]; then
	echo "ram-run: parsed load address '$BASE_RAW' is not a valid hex value -- refusing to guess." >&2
	exit 4
fi
BASE=$(printf '0x%X' "$BASE_RAW")
if (( BASE_RAW >= 0x80000000 )); then
	cat >&2 <<-EOF
	ram-run: '$ELF' is slot0/MRAM-linked (LOAD segment at $BASE) --
	         Flow C cannot RAM-run this: loading it at its resident address
	         just re-enters the ALREADY-RESIDENT MRAM image, not the freshly
	         built one, and JLinkExe will not warn you. Rebuild with the
	         ITCM retarget (see docs/aen-bench-bringup.md, Flow C) or use
	         Flow D (scripts/bench/aen/flash-jlink-mramxip.sh).
	EOF
	exit 5
fi
# Positive plausibility check: Flow C can only legitimately land at ITCM
# 0x0, the ITCM global alias (0x50000000 / 0x58000000), or SRAM
# (0x02xxxxxx). Anything else -- notably a DTCM address like 0x2000xxxx --
# means we picked the wrong LOAD segment (e.g. a .bss/data segment instead
# of the code segment) and would splat the image over live RAM if we loaded
# it. This is the check that would have caught the original bug.
if (( BASE_RAW != 0x0 && BASE_RAW != 0x50000000 && BASE_RAW != 0x58000000 &&
      (BASE_RAW < 0x02000000 || BASE_RAW >= 0x03000000) )); then
	cat >&2 <<-EOF
	ram-run: REFUSING to load -- derived base $BASE for '$ELF' is not a
	         plausible Flow C target (expected 0x0, the ITCM global alias
	         0x50000000/0x58000000, or SRAM 0x02xxxxxx). This looks like a
	         DTCM/data LOAD segment was picked instead of the code segment,
	         which would corrupt live RAM (the resident app's stack/bss) if
	         loaded. Check 'readelf -l $ELF' by hand before proceeding.
	EOF
	exit 6
fi

# SAFETY GATE -- confirm the AEN E8 is on the other end BEFORE loadbin+go.
#
# Flow C is not a read: it writes an AEN-linked image into ITCM and executes
# it. jlink_run's USB masking (above) already prevents opening any probe but
# BENCH_PLACE's; this is defense in depth for the case that masking didn't
# take, matching the DPIDR gate the MRAM writers have had since #1069.
cat > "$WORKDIR/preflight.jlink" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
jlink_run -nogui 1 -CommandFile "$WORKDIR/preflight.jlink" \
  > "$WORKDIR/preflight.out" 2>&1 || true
bench_jlink_assert_connected "$WORKDIR/preflight.out" "RAM-run preflight" || exit 7
bench_jlink_assert_aen_dpidr "$WORKDIR/preflight.out" "RAM-run preflight" || exit 4

# --- Session 1: LOAD + START the app, then disconnect leaving it RUNNING --
#
# Split from the read-back into a SEPARATE JLinkExe session (alp-sdk#2076).
# Bench-measured on e1m-aen-evk-02/-03 (2026-09-13): an in-session second
# `halt` issued after `go` + `Sleep` returned an INCOHERENT core
# (SP=0x00000030, FAULTMASK=378E, the FPS registers mirroring R0-R14, no
# MSPLIM block at all) and the `mem8` that followed it failed outright
# ("Could not read memory") -- on an app a SEPARATE, read-only attach
# proved seconds later was still running cleanly (fault-free core,
# IPSR=0/CFSR=0/BFAR=0, buffer read fine). No root cause is established
# (SE gating a running core, J-Link sequencing, or something else) -- this
# only avoids the shape measured broken and matches the shape measured to
# work.
#
# Ending on a plain `exit` after `go` -- not `qc`, and no `halt`/`r` first
# -- leaves the target running: BENCH-VERIFIED 2026-09-13 on
# e1m-aen-evk-02 and e1m-aen-evk-03 (examples/peripheral-io/blink, whose
# main() never returns; 6 of 6 clean runs, three per board). Every load
# transcript ended `loadbin ... O.K.`, `setpc`, `go`, `exit`; a separate
# attach immediately after found the core still running (`CPU is not
# halted.` after `go`) and the kernel's `curr_tick` advancing about 1s
# across a `Sleep 1000`. This is a direct measurement of `exit` after `go`
# on a core J-Link itself halted moments before -- NOT the same claim as
# flash-jlink.sh's `RSetType 2`/`r`/`g`/`exit` (scripts/bench/aen/flash-jlink.sh:128-131),
# where the PIN reset reboots the Secure Enclave and J-Link's own attach
# fails afterwards (docs/aen-bench-bringup.md:437-439, "Attach to CPU
# failed") -- there `g`/`exit` act on a core J-Link no longer controls,
# which shows closing J-Link does not stop an SE-booted core, not that
# `exit` after resuming a core J-Link itself halted leaves it running.
# Whether `qc` behaves the same as `exit` is NOT established and not
# claimed here.
{
  echo connect
  echo halt
  [ -n "$PRELOAD" ] && cat "$PRELOAD"
  echo "loadbin $BIN $BASE"
  echo "setpc $ENTRY"
  echo go
  echo exit
} > "$WORKDIR/load.jlink"
echo ">>> RAM-run $(basename "$BD")  entry=$ENTRY  base=$BASE  ram_console_buf=$BUF  sleep=${SLEEP}ms  place=$BENCH_PLACE" >&2
jlink_run -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommandFile "$WORKDIR/load.jlink" 2>"$WORKDIR/load.err" > "$WORKDIR/load.out" || true
# Same alp-sdk#1318 hazard as the read session below: a load+go that never
# reached the probe must not fall through into a session-2 read that then
# reports an EMPTY console and reads as a crashed app.
bench_jlink_assert_connected "$WORKDIR/load.out" "RAM-run $(basename "$BD") (load+go)" || exit 7
# A CONNECTED session can still fail the `loadbin` itself (e.g. a target RAM
# access rejected, a bus fault mid-write) -- DTCM survives the SYSRESETREQ
# `loadbin` triggers, so a stale image from an EARLIER run can boot instead
# and its console would be read as if THIS load had succeeded.
#
# POSITIVE check, not a "fail"/"error" substring scan: bench-measured
# 2026-09-13 (examples/peripheral-io/blink, 6 of 6 clean runs) that a
# successful RAM loadbin's transcript reads `loadbin ... O.K.` immediately
# after the command echo. A negative keyword scan was tried and dropped --
# `$BIN`/`$BD` is an operator-chosen path and a build directory legitimately
# named e.g. "ci-failover-build" makes the `loadbin <path> ...` echo itself
# match "fail", failing a load that actually succeeded.
if ! grep -A3 -i "^J-Link>loadbin " "$WORKDIR/load.out" | grep -qi "O\.K\."; then
	echo "!! ram-run: loadbin did not report 'O.K.' -- refusing to treat this as a" >&2
	echo "   fresh load (a STALE image already resident in ITCM/DTCM can otherwise" >&2
	echo "   boot and be read back as if this run's image had loaded)." >&2
	grep -A3 -i "^J-Link>loadbin " "$WORKDIR/load.out" | head -6 >&2
	exit 8
fi

# Host-side wait -- Sleep no longer runs inside a JLinkExe session, so the
# host waits the same $SLEEP milliseconds between the two sessions instead.
# Routing through jlink_run adds its own overhead per call (a labgrid
# `show`, an OpenOCD probe-firmware read) on top of this -- the app is
# guaranteed to run for AT LEAST sleep_ms, not exactly sleep_ms.
SLEEP_S=$(awk -v ms="$SLEEP" 'BEGIN { printf "%.3f", ms / 1000 }')
sleep "$SLEEP_S"

# --- Session 2: a FRESH, read-only attach ----------------------------------
#
# Not a halt-then-read inside session 1 -- that is exactly the shape
# measured broken above. No `halt` before `mem8` either: this bench's own
# Flow D read-back already reads `ram_console_buf` off a LIVE M55 core with
# a bare `connect` + `mem8`, no halt, bench-verified working
# (scripts/bench/aen/flash-jlink.sh:175-187, "attach read-only with the
# GENERIC device and dump the RAM console (the part-number profile can't
# re-halt the running secure core)"; docs/aen-bench-bringup.md:439-440,
# "memory reads work while the CPU runs; register reads error out
# harmlessly"). Adding a halt here would reintroduce the in-session halt
# this fix removes, just moved into the second session.
cat > "$WORKDIR/read.jlink" <<EOF
connect
mem8 $BUF, $SIZE
exit
EOF
jlink_run -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommandFile "$WORKDIR/read.jlink" 2>"$WORKDIR/read.err" > "$WORKDIR/read.out" || true
# JLinkExe exits 0 even when it never opened the probe, so the `|| true` above
# cannot be relied on. Without this the decoder below prints an EMPTY console
# block for a pure infrastructure failure, which reads as a crashed app
# (alp-sdk#1318). Fail before decoding, not after.
bench_jlink_assert_connected "$WORKDIR/read.out" "RAM-run $(basename "$BD") (read)" || exit 7
# A CONNECTED session can still fail the READ itself with no root cause
# established for why (the very defect this file fixes) -- `mem8` can
# report "Could not read memory." while every prior command in the same
# session succeeded, and bench_jlink_assert_connected above does not see
# that as a connect failure. Check the read's OWN outcome before decoding:
# an explicit read failure, or a transcript with no `ADDR = HH HH ...` dump
# line at all, must not decode as a silent empty console.
if grep -qi "Could not read memory" "$WORKDIR/read.out"; then
	echo "!! ram-run: mem8 reported 'Could not read memory' -- refusing to decode this as a console." >&2
	exit 9
fi
if ! grep -qE '^[0-9A-Fa-f]+ = ' "$WORKDIR/read.out"; then
	echo "!! ram-run: no memory dump line in the read session's transcript -- refusing to decode an empty read as a console." >&2
	exit 9
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
}' "$WORKDIR/read.out"
echo
echo "---------------------------------"
