#!/usr/bin/env bash
# scripts/bench/aen/ram-run.sh <build-dir> [sleep_ms] [bufsize_hex] [preload_jlink_file]
#
# Requires BENCH_PLACE (the labgrid-client place name whose probe to use, e.g.
# "e1m-aen-evk-02" -- you must already hold that place's reservation) AND
# AEN_JLINK_RUN (the board-farm's jlink-run.sh wrapper path -- no default;
# see the comments below for why). See the BENCH_PLACE/AEN_JLINK_RUN
# comments below.
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe via the board-farm's jlink-run.sh wrapper). Runs under
# WSL2 on Windows. No SETOOLS/SE-UART -- this flow never writes MRAM. See
# docs/aen-bench-bringup.md. Requires gawk (GNU awk) -- the LOAD-segment
# address derivation below uses strtonum(), a gawk extension mawk/BSD awk
# lack; checked explicitly before it's needed (see the "requires gawk"
# exit below).
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

# fd 3 -- a private copy of the ORIGINAL stderr, taken before anything else
# redirects the real fd 2. The EXIT trap's "leaving transcripts" message
# below writes here, not `>&2`, so it still reaches the terminal even if
# delivered while a `jlink_run ... > file 2>&1` call's own redirection is
# active (alp-sdk#2076 review round 3, finding 2).
exec 3>&2

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
# alp-sdk#2064) instead of a bare -SelectEmuBySN. Several probes on this
# bench share one OEM serial (the exact count enumerated has drifted before
# and is not repeated here); JLINK_SN alone cannot tell them apart, and the
# DPIDR preflight below only checks WHICH board answered THAT session -- it
# does not stop session 1 loading one board while session 2 reads a
# DIFFERENT one under the same shared serial. jlink-run.sh instead resolves
# the named place's probe from labgrid and USB-masks every other probe so
# only it can be opened, closing that gap structurally rather than by
# checking after the fact. No default -- host/bench-specific, like
# SE_UART/AEN_OPENOCD_CFG in bench-env.sh (NOT edited here: PR #2080 and
# draft #2033 both touch that file).
BENCH_PLACE="${BENCH_PLACE:-}"
if [ -z "$BENCH_PLACE" ]; then
	echo "ram-run: BENCH_PLACE is unset. Several J-Links on this bench share" >&2
	echo "         one OEM serial -- a bare -SelectEmuBySN cannot tell them" >&2
	echo "         apart, so ram-run.sh requires the labgrid-client PLACE NAME" >&2
	echo "         whose probe to use, e.g.:" >&2
	echo "             export BENCH_PLACE=e1m-aen-evk-02" >&2
	echo "         (you must already hold that place's labgrid reservation)." >&2
	exit 2
fi

# AEN_JLINK_RUN -- the board-farm's per-probe isolation wrapper (labgrid
# reservation check + USB-namespace masking + firmware-safe JLinkExe
# selection). NO default, matching AEN_OPENOCD_CFG's own "error-if-unset"
# shape in bench-env.sh, NOT bench_jlink_exe()'s $HOME/segger-latest
# default: on a real bench host the natural default IS the real wrapper, so
# a dev/test run that forgot to override this would silently drive a real
# probe instead of failing closed (alp-sdk#2076 review). Export it
# explicitly, or point it at a fake sharing the wrapper's own "<place>
# [JLinkExe args...]" contract in a test.
AEN_JLINK_RUN="${AEN_JLINK_RUN:-}"
if [ -z "$AEN_JLINK_RUN" ]; then
	echo "ram-run: AEN_JLINK_RUN is unset. This is the board-farm's per-probe" >&2
	echo "         isolation wrapper (host-specific, not shipped). Export it, e.g.:" >&2
	echo "             export AEN_JLINK_RUN=\$HOME/board-farm/bin/jlink-run.sh" >&2
	exit 2
fi
if [ ! -x "$AEN_JLINK_RUN" ]; then
	echo "ram-run: '$AEN_JLINK_RUN' not found or not executable." >&2
	exit 2
fi

# JLINK_SN selection -- for the SIX sibling scripts in this directory that
# still select their probe this way and point HERE for the explanation
# (flash-jlink.sh, flash-jlink-hp.sh, flash-run.sh, flash-all-flowd.sh,
# erase-storage.sh, reread.sh; NOT flash-jlink-mramxip.sh -- it cites this
# file only for the #935 BUF_SYM guard and uses its own `${JLINK_SN:+...}`
# pattern, not this one): WHY their `-SelectEmuBySN` is added only when
# JLINK_SN is set, never unconditionally.
# Leaving JLINK_SN unset is NOT a no-op -- alplab-gw carries multiple
# J-Links, some sharing a cloned OEM serial, and an unselected JLinkExe run
# there fails every command with "Cannot connect to the probe/programmer"
# (alp-sdk#1318) or silently attaches the wrong one; forcing an EMPTY
# -SelectEmuBySN unconditionally would be worse than omitting the flag, so
# those scripts guard it with `[ -n "${JLINK_SN:-}" ]`. ram-run.sh ITSELF no
# longer selects by JLINK_SN -- see BENCH_PLACE/AEN_JLINK_RUN above, which
# route through jlink-run.sh's USB masking instead, closing the wrong-board
# gap a bare serial select cannot (alp-sdk#2076 review, alp-sdk#2064).

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

# _connect_or_exit <transcript> <context> -- bench_jlink_assert_connected
# (bench-env.sh), plus a ram-run-specific correction. bench-env.sh's own
# hint on a failed connect says `export JLINK_SN=<serial>`, which this
# script now ignores (BENCH_PLACE/AEN_JLINK_RUN above select the probe
# instead) -- bench-env.sh is off limits (PR #2080/#2033 both touch it), so
# the correction is appended here rather than editing that hint in place.
_connect_or_exit() {
	local out="$1" ctx="$2"
	if ! bench_jlink_assert_connected "$out" "$ctx"; then
		echo "ram-run: (the JLINK_SN hint above is stale for this script --" >&2
		echo "         set BENCH_PLACE to a held labgrid-client place name" >&2
		echo "         instead; AEN_JLINK_RUN routes through it.)" >&2
		exit 7
	fi
}

OBJ="$(bench_tool_prefix)" || exit $?
# gawk required -- the LOAD-segment derivation below (BASE_RAW) uses
# strtonum(), a GNU-awk extension absent from mawk/BSD awk; those instead
# fail with "function strtonum never defined" and, pre-this-check, that
# fell through to the misleading "could not find a LOAD segment" (exit 4)
# below (alp-sdk#2076 review round 3, finding 4). Checked here (after the
# BENCH_PLACE/AEN_JLINK_RUN/sleep_ms gates above, all of which must keep
# exiting 2 on their own bad input regardless of which awk is installed)
# and before the first strtonum() call.
if ! awk 'BEGIN{strtonum("0")}' </dev/null >/dev/null 2>&1; then
	echo "ram-run: the system 'awk' has no strtonum() (e.g. mawk) -- this" >&2
	echo "         script requires gawk (GNU awk) for the LOAD-segment" >&2
	echo "         address parsing below. Install/select gawk and retry." >&2
	exit 1
fi
ELF="$BD/zephyr/zephyr.elf"
BIN="$BD/zephyr/zephyr.bin"
# A missing/mistyped build-dir must say so plainly, not fall through to the
# UART-console message below. Without this check, `readelf -h` on a
# nonexistent ELF fails but the pipeline's exit status is awk's (0, empty
# output) -- BUF_SYM then comes back empty too and exit 3's "rebuild with
# the RAM console" advice fires for a build that was never built at all
# (alp-sdk#2076 review round 3, finding 9).
if [ ! -f "$ELF" ]; then
	echo "ram-run: '$ELF' does not exist -- is '$BD' a real build dir (west build -d <build-dir>)?" >&2
	exit 1
fi
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

# WORKDIR -- every JLinkExe transcript and generated CommandFile for this run
# lives under one mktemp -d directory (under $TMPDIR, matching
# openocd-ram-run.sh's own preflight-transcript mktemp), not fixed /tmp
# names. Fixed names (the old /tmp/jlink.out, /tmp/ram-run-preflight.out)
# let one run's transcript be overwritten by a CONCURRENT run on this same
# host moments before it is decoded -- reproduced running this script's own
# test suite against a real bench session: a pytest run's fake transcript
# was still sitting at /tmp/jlink-read.out after the test process exited.
#
# Created here, right before the FIRST transcript write, not up near the top
# of the script -- every earlier exit (1/2/3/4/5/6 above) fires before any
# transcript exists, so creating WORKDIR that early only left an empty
# ram-run.XXXXXX dir behind on those paths, plus a "leaving transcripts
# under ..." message with nothing under it to inspect (alp-sdk#2076 review
# round 3, finding 6).
#
# Removed only when $RUN_OK is set (just before the final success output,
# near the bottom of this script) -- NOT keyed off $? (rc=0). An earlier
# version of this fix added `trap 'exit N' INT/TERM` so a killed run would
# still reach this EXIT trap with a known-good `$?`; review round 3
# MEASURED (this script's own signal harness, plus a bare `sleep 20` with
# no traps at all) that this regressed things and fixed nothing real:
#   - a signal to the whole PROCESS GROUP -- a terminal Ctrl-C, or a
#     `kill`/`pkill` targeting the group, the realistic case -- reaches the
#     running child directly and kills this script (EXIT trap included)
#     within milliseconds, trap or no trap. But `trap 'exit 130' INT` turns
#     that into an ORDINARY `exit 130` as far as a calling `for i in 1 2; do
#     ram-run.sh ...; done` loop is concerned -- its own Ctrl-C-abort check
#     fires only on a child actually KILLED BY SIGINT, not one that merely
#     exited 130, so it didn't stop (iteration 2 still ran a real load+go).
#     Dropping the trap fixes this: this script now dies BY the signal like
#     any other untrapped command (confirmed: a group SIGINT during a
#     loop's first iteration now stops it before iteration 2).
#   - a signal landing mid `jlink_run` wrote the "leaving transcripts" line
#     into THAT call's own `> load.out 2>&1` redirection, not the terminal
#     -- unrelated to the trap itself; fixed by fd 3 above regardless.
#   - bash still runs the EXIT trap on an untrapped, signal-terminated exit
#     (measured: WORKDIR is correctly retained with no INT/TERM trap at
#     all), so no INT/TERM trap is needed for that either.
# NOT fixed by any trap policy, because it isn't caused by one: a signal
# sent to ONLY this script's own pid (`kill <pid>`, not through job
# control) while it is blocked on a foreground child is deferred BY BASH
# ITSELF until that child returns -- measured ~6s against a 6s foreground
# `sleep`/`jlink_run` call, identical with the trap present or absent, and
# reproduced with a bare two-line `sleep 20` script carrying no traps
# whatsoever. Fixing that needs a background watcher forwarding the signal
# to the actual child -- real machinery this script chooses not to add; a
# real Ctrl-C or a process-group kill, the normal ways to stop a bench
# script, are unaffected. `$?` at EXIT time has no well-defined "success"
# value when the shell died from a signal -- checking $RUN_OK instead of
# `$?` is what makes retention correct either way.
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/ram-run.XXXXXX")
_ram_run_cleanup() {
	if [ -n "${RUN_OK:-}" ]; then
		rm -rf "$WORKDIR"
	else
		echo "ram-run: leaving transcripts under $WORKDIR for inspection." >&3
	fi
}
trap _ram_run_cleanup EXIT

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
_connect_or_exit "$WORKDIR/preflight.out" "RAM-run preflight"
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
# stderr merged into the same transcript as the preflight already does --
# jlink-run.sh reports every one of ITS OWN refusals (place not held, board
# unpowered, USB mask failure, a failed labgrid `show`) on stderr only, and
# a separate, silently-dropped .err file turned those into a misleading
# "produced no J-Link output at all" from bench_jlink_assert_connected
# instead of the wrapper's own, more specific message (alp-sdk#2076 review).
jlink_run -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommandFile "$WORKDIR/load.jlink" > "$WORKDIR/load.out" 2>&1 || true
# Same alp-sdk#1318 hazard as the read session below: a load+go that never
# reached the probe must not fall through into a session-2 read that then
# reports an EMPTY console and reads as a crashed app.
_connect_or_exit "$WORKDIR/load.out" "RAM-run $(basename "$BD") (load+go)"

# Session-1 COMPLETENESS gate (alp-sdk#2076 review round 3, finding 1/3) --
# _connect_or_exit above only proves the J-Link session reached SOME
# command prompt; it does not prove `loadbin`, `setpc`, `go`, and `exit` all
# ran to completion, and judging success on the `loadbin` window alone let a
# crashed or truncated session pass (real bench evidence: a JLinkExe process
# crashing mid-session, around the `S/N`/`License(s)` banner just after
# `connect`, produced a transcript with no `loadbin` line at all -- the
# pre-fix check reported that as "loadbin did not report 'O.K.'", which is
# wrong: the session never reached loadbin). On silicon the core stays
# halted at whatever `loadbin`/`setpc`/`go` last reached before a crash,
# DTCM/`ram_console_buf` survives it, and session 2 then reads the PREVIOUS
# run's console as this run's output -- the exact stale-console case exit 8
# exists to prevent.
#
# JLinkExe prints one line -- `Script processing completed.` -- and ONLY
# once it has finished running every command in the CommandFile, including
# the final `exit`; nothing that happens to the process afterwards changes
# that. Verified against 5 real V9.74 transcripts (present, verbatim, in
# every one) and against the crash/truncation shapes review round 3
# measured: a transcript cut right after `O.K.`, a transcript cut right
# after `go` (with or without the calling shell's own "Aborted (core
# dumped)"/"Killed" appended), and a crash before `loadbin` ever ran -- NONE
# of them print this line, no matter where the crash lands, so this one
# check replaces separately hunting a `Segmentation fault` string (which
# only one crash shape produces) and separately confirming each of
# `loadbin`/`setpc`/`go` was individually echoed (implied by this line's
# presence -- JLinkExe cannot print it without having processed all of
# them).
if ! grep -qF 'Script processing completed.' "$WORKDIR/load.out"; then
	echo "!! ram-run: session 1's transcript has no 'Script processing" >&2
	echo "   completed.' line -- the J-Link process crashed, was killed, or" >&2
	echo "   was truncated before finishing (this can happen at any point," >&2
	echo "   including right after 'go'); this is NOT a reported" >&2
	echo "   loadbin/setpc/go failure, just an incomplete session." >&2
	echo "   Full transcript: $WORKDIR/load.out" >&2
	exit 10
fi

# _session1_window <exact-echo-line> <transcript> -- print the lines
# between the LAST occurrence of the exact echoed command and the next
# `J-Link>` prompt. LAST, not first: a preload file that loads the SAME
# $BIN at the SAME $BASE ahead of the main load (a legitimate use) would
# otherwise let ITS successful window satisfy the check for the main
# image's own (review finding 2). Passed through ENVIRON, not awk `-v`: a
# literal backslash/tab byte sequence in $BIN would otherwise be
# awk-interpreted by `-v` (review finding 7, nit). Relies on gawk (checked
# above), but does not itself need strtonum().
_session1_window() {
	local echo_line="$1" transcript="$2"
	ECHO_LINE="$echo_line" awk '
	  $0 == ENVIRON["ECHO_LINE"] { f = 1; win = ""; next }
	  f && /^J-Link>/ { f = 0 }
	  f { win = win $0 "\n" }
	  END { printf "%s", win }
	' "$transcript"
}

# A session that completed can still fail `loadbin` ITSELF (e.g. a target
# RAM access rejected, a bus fault mid-write) -- DTCM survives the
# SYSRESETREQ `loadbin` triggers, so a stale image from an EARLIER run can
# boot instead and its console would be read as if THIS load had succeeded.
# JLinkExe does not abort the rest of the CommandFile on a failed `loadbin`
# either (it still runs `setpc`/`go`/`exit` and prints "Script processing
# completed." regardless), so the completeness gate above can't catch this
# -- it needs its own check.
#
# POSITIVE check, scoped to an EXACT window, not a whole-transcript
# substring scan -- both were bench-measured wrong on evk-02/evk-03
# (2026-09-13, real JLinkExe V9.74 through jlink-run.sh):
#   - too narrow: a real successful loadbin puts SIX lines (the implicit-
#     reset banner + "Downloading file [...]...") between the echoed
#     command and "O.K.", not "immediately after" as an earlier draft of
#     this comment claimed -- a fixed `grep -A3` window missed it and
#     exited 8 on every one of 6 clean runs across both boards.
#   - too loose: a case-insensitive `fail|error` scan (or an unscoped
#     "any O.K. after any loadbin" check) matches the OPERATOR'S OWN PATH
#     ($BIN/$BD are chosen by the caller, e.g. a directory named
#     "demo.k.build" or "ci-failover-build") or a DIFFERENT loadbin
#     entirely (an unrelated one in $PRELOAD) -- either direction reports
#     a stale/failed load as this run's own success or failure.
# Anchored on the EXACT echoed command for THIS image (`$BIN $BASE`, not a
# pattern), scanning only the lines up to the NEXT `J-Link>` prompt, and
# requiring one of them to be the line `O.K.` verbatim (case-sensitive,
# not a substring) -- the real success marker JLinkExe itself prints,
# nothing weaker.
LOADBIN_ECHO="J-Link>loadbin $BIN $BASE"
if ! _session1_window "$LOADBIN_ECHO" "$WORKDIR/load.out" | grep -qx 'O\.K\.'; then
	echo "!! ram-run: loadbin did not report 'O.K.' -- refusing to treat this as a" >&2
	echo "   fresh load (a STALE image already resident in ITCM/DTCM can otherwise" >&2
	echo "   boot and be read back as if this run's image had loaded)." >&2
	_session1_window "$LOADBIN_ECHO" "$WORKDIR/load.out" >&2
	exit 8
fi

# setpc/go succeed SILENTLY on real JLinkExe V9.74, verified against 5 real
# transcripts: `setpc`'s own window is EMPTY (it prints nothing), and
# `go`'s window contains ONLY the one-line "Memory map '...' is active"
# banner -- ANYTHING else is a rejection. This replaces an earlier
# generic-keyword heuristic (`unknown command|invalid|cannot|fail`) that
# never fired on either of JLinkExe's REAL rejection strings -- `Syntax:
# SetPC <addr>` (bad/missing argument) and `CPU is not halted !` (setpc
# issued against a running core) -- because neither contains any of those
# words (review round 3, finding 4).
SETPC_WINDOW="$(_session1_window "J-Link>setpc $ENTRY" "$WORKDIR/load.out")"
if [ -n "$SETPC_WINDOW" ]; then
	echo "!! ram-run: 'setpc $ENTRY' was rejected:" >&2
	printf '%s\n' "$SETPC_WINDOW" >&2
	exit 11
fi
GO_WINDOW="$(_session1_window "J-Link>go" "$WORKDIR/load.out")"
if printf '%s\n' "$GO_WINDOW" | grep -vqE "^\$|^Memory map '.*' is active\$"; then
	echo "!! ram-run: 'go' did not report the expected 'Memory map ... is" >&2
	echo "   active' banner -- treating as rejected:" >&2
	printf '%s\n' "$GO_WINDOW" >&2
	exit 11
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
# stderr merged, same reason as session 1 above.
jlink_run -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommandFile "$WORKDIR/read.jlink" > "$WORKDIR/read.out" 2>&1 || true
# JLinkExe exits 0 even when it never opened the probe, so the `|| true` above
# cannot be relied on. Without this the decoder below prints an EMPTY console
# block for a pure infrastructure failure, which reads as a crashed app
# (alp-sdk#1318). Fail before decoding, not after.
_connect_or_exit "$WORKDIR/read.out" "RAM-run $(basename "$BD") (read)"
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
RUN_OK=1
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
