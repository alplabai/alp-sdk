#!/usr/bin/env bash
# scripts/bench/aen/openocd-ram-run.sh <build-dir> [core] [openocd_cfg]
#
# Cross-platform scope: Linux-side bench helper (drives OpenOCD against the
# board-farm's shared SWD config). Runs under WSL2 on Windows. See
# docs/aen-bench-bringup.md and scripts/bench/aen/README.md.
#
# OpenOCD counterpart to ram-run.sh's Flow C (ITCM RAM-run, no MRAM write) --
# but able to address EITHER of the E8's two M55 cores, which ram-run.sh's
# JLinkExe generic-Cortex-M55 attach cannot: it always lands on whichever
# core the shared SW-DP happens to expose as "the live core", which an
# entire bench campaign spent unknowingly targeting the WRONG one (alp-sdk#2037).
#
# BENCH-VERIFIED 2026-09-09 (alp-sdk#2037): on e1m-aen-evk-01 the two M55
# cores are separate CoreSight access ports on the ONE shared SW-DP --
#   M55-HP  AP 0x00200000
#   M55-HE  AP 0x00300000
# The board-farm's shared config (NOT part of this repo, already corrected +
# hardware-verified -- see AEN_OPENOCD_CFG below) declares both:
#   target create alif.m55he cortex_m -dap alif.dap -ap-num 0x00300000 -defer-examine
#   target create alif.m55hp cortex_m -dap alif.dap -ap-num 0x00200000
# HP is created LAST on purpose (OpenOCD makes the last-created target
# current), so a plain `init` addresses HP exactly as every flow on this
# bench has always (if unknowingly) done. HE carries -defer-examine so a
# plain `init` does not attach to it or write C_DEBUGEN -- reaching it needs
# the two extra commands below, which is why 'core' here defaults to "hp":
# THE DEFAULT DOES NOT CHANGE. Pass "he" explicitly to opt in.
#
# Addresses themselves do NOT change per core: both cores see instruction
# memory at 0x00000000 and data memory at 0x20000000 (only the EXTERNAL
# aliases differ -- 0x58000000 HE / 0x50000000 HP -- and those matter only
# for one core reading the other, not for this script).
#
# HE HAZARD -- NOT SILENT: on evk-01, the HE ITCM carries a resident ~4.6 KB
# stub the Secure Enclave parks there at boot (MSP=0x20040000, reset vector
# 0x00000B58, every fault vector collapsed onto one handler; fully captured
# at <board-farm>/artifacts/evk01-he-itcm-resident-FULL-2026-09-09.bin, md5
# d176f1864a94481c7af3c762d70462a2). `core=he` OVERWRITES it via load_image.
# Instruction memory is RAM, so a power cycle restores it -- this is not a
# persistent MRAM write like Flow A/D -- but it is not undone by this script,
# so the warning below is printed every time, not just the first.
#
# SAFETY GATE (alp-sdk#2037) -- this script landed today invoking OpenOCD
# directly with no USB path, no labgrid, and no DPIDR check: exactly the
# `ram-run.sh` hazard #1312/#1318 already fixed for JLinkExe, reintroduced
# here because OpenOCD is a different tool with no shared preflight. Three
# probes on alplab-gw answer the SAME cloned serial (AEN E8 3-4.4.3, the
# GD32 bridge 3-4.2, both `603000869`); with nothing pinning the path,
# OpenOCD picks one arbitrarily, and this script's first hardware actions
# are `halt` then `load_image ... 0x0` -- on the wrong board, that halts and
# overwrites a target this reservation does not cover. Caught before any
# hardware ran. Two independent layers now gate that, same as every MRAM
# writer in this directory:
#   1. AEN_OPENOCD_USB_LOCATION (bench-env.sh, bench_require_openocd) pins
#      the probe by labgrid-resolved USB path -- `adapter usb location`,
#      prepended on the command line per the shared config's own header
#      (that file must NOT hardcode it; labgrid's OpenOCDDriver supplies it
#      for a real run, a by-hand exporter run prepends it here instead).
#   2. A read-only preflight `init; shutdown` (before ANY halt/load_image)
#      captures OpenOCD's own DPIDR line and checks it with the SAME
#      bench_jlink_assert_aen_dpidr() the JLink flows use (bench-env.sh) --
#      the AEN E8 answers 0x4C013477, the GD32 bridge 0x0BE12477. Either
#      gate failing aborts before the probe is touched again.
#
# UNEXERCISED ON HARDWARE as of this change -- shellcheck + check_local_paths.py
# only, no board run. A real bench run should print, in order:
#   >>> openocd-ram-run preflight (M55-HE / M55-HP)  usb=<AEN_OPENOCD_USB_LOCATION>
# then OpenOCD's own DPIDR line containing 0x4c013477 (lower/upper-case
# either way -- the gate matches case-insensitively), THEN:
#   >>> openocd-ram-run <name>  core=M55-HE (AP 0x00300000)  msp=0x... pc=0x...
# (or M55-HP / AP 0x00200000 for the default), and OpenOCD's own transcript
# should show "target halted" after `resume` is issued -- the app then runs
# from ITCM exactly as a Flow C JLinkExe run would, just on the SELECTED core.
# A wrong-board probe should instead print the ABORT from
# bench_jlink_assert_aen_dpidr and exit 4, with no halt/load_image line ever
# appearing in the transcript.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
CORE="${2:-hp}"
CFG="${3:-$AEN_OPENOCD_CFG}"

if [ -z "$BD" ]; then
	echo "usage: openocd-ram-run.sh <build-dir> [core: hp(default)|he] [openocd_cfg]" >&2
	exit 1
fi
case "$CORE" in
	hp|he) : ;;
	*)
		echo "openocd-ram-run: core must be 'hp' (default) or 'he', got '$CORE'" >&2
		exit 1
		;;
esac

bench_require_openocd "$CFG" || exit $?
BIN="$BD/zephyr/zephyr.bin"
if [ ! -f "$BIN" ]; then
	echo "openocd-ram-run: no '$BIN' -- build the app first (scripts/bench/aen/build.sh)." >&2
	exit 1
fi

# Vector table word[0] = initial MSP, word[1] = initial PC (thumb bit set) --
# same convention ram-run.sh (setpc) and flash-jlink-mramxip.sh (reset-vector
# sanity check) already use for this bench, just read here instead of via
# readelf's entry point (load_image, unlike a JLink `go`, does not set PC).
read -r WORD0 WORD1 <<<"$(xxd -e -l 8 "$BIN" | awk '{print $2, $3}')"
if [ -z "$WORD0" ] || [ -z "$WORD1" ]; then
	echo "openocd-ram-run: could not read the first two words of '$BIN' -- refusing to guess MSP/PC." >&2
	exit 1
fi
MSP="0x$WORD0"
PC=$(printf '0x%X' $(( 0x$WORD1 & ~1 )))   # clear the thumb bit

# Same guard ram-run.sh applies before it will `go`: a slot0/MRAM-linked
# image's reset vector is 0x8xxxxxxx. Loading THAT at ITCM 0x0 here would not
# re-enter a resident MRAM image the way ram-run.sh's mis-load would -- it
# would just run garbage -- but it is exactly the same avoidable mistake, so
# refuse for the same reason: rebuild with the Flow C ITCM retarget first.
if (( 0x$WORD1 >= 0x80000000 )); then
	cat >&2 <<-EOF
	openocd-ram-run: '$BIN' looks slot0/MRAM-linked (word[1]=0x$WORD1) --
	                 this is a Flow C style ITCM RAM-run (load address 0x0).
	                 Rebuild with the Flow C ITCM retarget (see ram-run.sh /
	                 docs/aen-bench-bringup.md, Flow C) before using this script.
	EOF
	exit 1
fi

NAME=$(basename "$BD")
if [ "$CORE" = "he" ]; then
	AP="0x00300000"
	NAMEFMT="M55-HE"
	SELECT_CMDS="targets alif.m55he; alif.m55he arp_examine; "
	cat >&2 <<-EOF
	!! core=he: this OVERWRITES evk-01's resident HE ITCM stub (the ~4.6 KB
	!! Secure-Enclave park stub, MSP=0x20040000, reset vector 0x00000B58) via
	!! load_image. It is RAM, so a power cycle restores it -- but this script
	!! does not restore it for you, and does not ask before doing this.
	!! Captured backup: <board-farm>/artifacts/evk01-he-itcm-resident-FULL-2026-09-09.bin
	!! (md5 d176f1864a94481c7af3c762d70462a2).
	EOF
else
	AP="0x00200000"
	NAMEFMT="M55-HP"
	SELECT_CMDS=""
fi

# Print which core, explicitly and unambiguously, BEFORE touching the probe.
# This is the fix for the exact defect that caused alp-sdk#2037: OpenOCD's
# own per-line output is tagged with the TARGET NAME ("[alif.m55he]"), not
# the core it actually maps to, so a whole campaign's transcripts all said
# "m55he" while every one of them drove the HP. This line names the AP, the
# one thing that is unambiguous.
echo ">>> openocd-ram-run $NAME  core=$NAMEFMT (AP $AP)  msp=$MSP pc=$PC" >&2

# `adapter usb location` pins the probe to the labgrid-resolved USB path --
# it must run before `init` opens the adapter, and it is prepended here per
# the shared config's own header (never hardcoded into that file). See the
# SAFETY GATE comment at the top of this file.
USB_LOC_CMD="adapter usb location $AEN_OPENOCD_USB_LOCATION; "

# SAFETY GATE step 2/2 -- read-only preflight (init + shutdown, no halt, no
# load_image) BEFORE touching the probe for real. DPIDR is a per-SW-DP
# register, so examining the default target (HP, since only HE carries
# -defer-examine) is enough to surface it regardless of which core CORE
# ultimately selects.
echo ">>> openocd-ram-run preflight ($NAMEFMT)  usb=$AEN_OPENOCD_USB_LOCATION" >&2
PREFLIGHT_OUT=/tmp/openocd-ram-run-preflight.out
openocd -f "$CFG" -c "${USB_LOC_CMD}init; shutdown" >"$PREFLIGHT_OUT" 2>&1 || true
bench_jlink_assert_aen_dpidr "$PREFLIGHT_OUT" "openocd-ram-run preflight ($NAMEFMT)" || exit 4

CMDS="${USB_LOC_CMD}init; ${SELECT_CMDS}halt; load_image $BIN 0x0 bin; reg msplim_s 0x00000000; reg msplim_ns 0x00000000; reg msp $MSP; reg pc $PC; resume; shutdown"

openocd -f "$CFG" -c "$CMDS"
