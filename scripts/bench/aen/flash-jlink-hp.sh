#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-hp.sh [--replace-atoc] [--atoc-unqueryable] <build-dir> [sram0_beacon_addr_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh; drives
# JLinkExe + the Alif SETOOLS, both Linux binaries on this bench). Runs under
# WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D, but for the M55-HP (RTSS-HP) core -- the SECOND M55.  Every other
# helper here targets the HE core (cpu_id M55_HE, loadAddress 0x58000000); this
# one authors an M55_HP ATOC (cpu_id M55_HP, loadAddress 0x50000000 = the HP ITCM
# global base) so the SES releases + boots the HP core, which is held in reset at
# power-on (the J-Link AP map shows only the HE core's AP with a readable CPUID).
#
# Observation: the bench reads memory over SWD via the HE/AXI debug AP, not the
# HP core's AP, so this does NOT depend on the HP RAM console being reachable.
# The HP app (examples/aen/aen-hp-core-smoke) writes a LIVENESS BEACON to global
# SRAM0 (0x02000000, always-on, master-agnostic): [0]=magic 0xA11FE000,
# [1]=CPUID, [2]=VTOR, [3]=heartbeat.  This helper reads the beacon, then re-reads
# the heartbeat word -- an ADVANCING heartbeat proves the HP core is actively
# executing (not a stale value from a prior image).
#
# GOTCHA -- returning to the canonical self-test: an HP ATOC becomes the active
# boot image, so the SES boots HP instead of the slot0 person_detect.  After HP
# bring-up, re-flash the canonical image:
#   flash-jlink-mramxip.sh <person_detect-build-dir>
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

# #2025/#2027 -- Flow D has no SE-UART BY DESIGN ("J-Link only, no serial
# device required"), so when one is not exported it cannot query the
# resident ATOC the way the Flow A guard (bench_atoc_replace_guard,
# scripts/bench/aen/bench-env.sh) does before a J-Link `loadbin` REPLACES it.
# --atoc-unqueryable is a deliberate, differently-named acknowledgement for
# THAT case, NOT the Flow A `--replace-atoc` opt-out: an operator on a
# no-SE-UART slot (e.g. an AEN EVK bench place with no SE-UART wired) will pass this on every single Flow D
# run, and that habit must never also silence Flow A's guard on a board
# where the resident TOC genuinely can be read. Do not merge or alias the
# two flags. --replace-atoc is also accepted here, for the OTHER case: a
# bench slot that DOES have an SE-UART wired -- see bench_flowd_atoc_guard
# in bench-env.sh, which runs the real shared guard whenever $SE_UART is
# exported and usable instead of requiring a blind acknowledgement here.
#
# PARSER SHAPE (review MINOR 5, alp-sdk#2027) -- deliberately kept as the
# whole-argv `for` scan #2029 already used for --atoc-unqueryable, NOT
# flash-run.sh's `while`/`shift` loop. That means `--replace-atoc` is
# recognised no matter where it lands in argv (e.g. `<build-dir>
# --replace-atoc` still enables it here), where flash-run.sh's parser stops
# honouring flags at the first non-option token -- the SAME invocation shape
# does NOT enable it there. This is MORE permissive than Flow A on the one
# flag that disables the resident-ATOC check entirely. Kept this way, not
# fixed, so both Flow D flags share ONE parsing convention in this loop
# rather than splitting the unchanged --atoc-unqueryable from a newly
# stricter --replace-atoc; if this permissiveness is ever exploited by
# accident (a flag landing after <build-dir> unintentionally), switch this
# whole loop to flash-run.sh's while/shift shape, not just this one flag.
REPLACE_ATOC=0
ATOC_UNQUERYABLE=0
POSITIONAL=()
for arg in "$@"; do
  case "$arg" in
    --replace-atoc) REPLACE_ATOC=1 ;;
    --atoc-unqueryable) ATOC_UNQUERYABLE=1 ;;
    *) POSITIONAL+=("$arg") ;;
  esac
done
set -- "${POSITIONAL[@]}"

BD="$1"
BEACON="${2:-0x02000000}"
bench_require_setools || exit $?

# GUARD (alp-sdk#2027) -- resolved over $SE_UART alone, well before this
# script's own first J-Link touch (step 0 below), so the query (and any
# reset it might trigger) has settled before the probe is involved at all.
bench_flowd_atoc_guard "$REPLACE_ATOC" "$ATOC_UNQUERYABLE" flash-jlink-hp HP-APP || exit $?

SET="$SETOOLS_DIR"
# OBJ itself is unused (this flow reads the beacon via J-Link `mem32`, not
# $OBJ-nm/-readelf); the assignment is kept as a preflight -- `|| exit $?`
# fails fast if the arm-zephyr-eabi toolchain doesn't resolve, matching
# every sibling flash-jlink*.sh.
# shellcheck disable=SC2034
OBJ="$(bench_tool_prefix)" || exit $?
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns.
JLINK_ARGS=(bench_jlink_run)
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"

# 0. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. Same DPIDR gate as
# flash-jlink-mramxip.sh (see that script for the full rationale): JLINK_SN
# narrows probe choice but does not itself prove which board answered. Hard
# ABORT, not a warning -- read-only connect first, no writes until confirmed.
cat > /tmp/hp-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/hp-preflight.jlink \
  > /tmp/hp-preflight.out 2>&1 || true
bench_jlink_assert_aen_dpidr /tmp/hp-preflight.out "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# 1. stage the HP image + an M55_HP signed-ATOC config (cpu_id/loadAddress are
#    the HP core's, the one structural difference from the HE flash configs).
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "HP-APP":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] }
}
JSON

cd "$SET"
echo ">>> FLOW-D M55_HP flash $NAME  (SRAM0 beacon=$BEACON)" >&2
./app-gen-toc -f "build/config/$NAME.json" >/tmp/hp-gentoc.log 2>&1 || { echo "gen-toc FAILED"; tail /tmp/hp-gentoc.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ADDR" >&2

# SECTOR-PAD (alp-sdk#2233): the built-in loader rewrites the WHOLE 16 KiB
# sector(s) $PKG touches and never reads their prior contents first -- see
# bench-env.sh's Flow D section header. Read those sectors' current MRAM
# content and overlay $PKG on them; the padded image is what gets loadbin'ed.
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-flash-jlink-hp-XXXXXX")" || exit 9
bench_flowd_prepare_write flash-jlink-hp "$FLOWD_SCRATCH" "$PKG:$ADDR" || exit 9

# 2. part-number device unlocks the MRAM loader; write the sector-padded
#    package, then PIN reset so the SE boot ROM reloads + boots the HP ATOC.
#    `verifybin` is deliberately GONE (#2233): it only ever compared against
#    J-Link's own flash cache, never a fresh chip read -- see the
#    bench_flowd_proof step below.
cat > /tmp/hp-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)
RSetType 2
r
g
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/hp-write.jlink 2>&1 | tee /tmp/hp-write.out | \
  grep -iE "could not connect|fail|error|Verify|O\.K\.|Reset" | head -20
if grep -qi "Could not connect to the target device" /tmp/hp-write.out; then
  echo "!! $JLINK_DEVICE_FLASH profile FAILED to connect -- flow D not unlocked on this probe."
  exit 2
fi

# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1343/#1488 verifybin
# gate) -- a FRESH read-only J-Link session savebin's every padded range back
# and cmp's it byte-for-byte against the padded image, proving both that $PKG
# landed AND that its sector neighbours survived.
if ! bench_flowd_proof flash-jlink-hp "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
  echo "!! READ-BACK PROOF FAILED -- MRAM does NOT match the padded image for $PKG @ $ADDR."
  echo "   Do not treat this board as flashed."
  exit 3
fi
echo "verify: read-back proof OK ($PKG @ $ADDR, sector-padded)"

# 3. SES has booted the HP core; read the SRAM0 beacon via the generic device
#    (the HE/system AP reads global SRAM0 regardless of HP core state), then
#    re-read the heartbeat word after a delay to show it advancing.
sleep 3
HB=$(printf "0x%X" $(( BEACON + 12 )))   # beacon[3] = heartbeat
cat > /tmp/hp-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 $BEACON, 0x10
Sleep 400
mem32 $HB, 0x4
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/hp-read.jlink 2>/tmp/hp-read.err > /tmp/hp-read.out || true
# JLinkExe exits 0 even when it never opened the probe, so `|| true` above
# hides a total connect failure and the decode below would render it as
# empty target output (alp-sdk#1318).
bench_jlink_assert_connected /tmp/hp-read.out "Flow D HP read-back" || exit 7
echo "----- $NAME M55-HP SRAM0 beacon (magic / CPUID / VTOR / heartbeat) -----"
grep -iE "^$(printf '%08X' $BEACON)| = " /tmp/hp-read.out | head
echo "(heartbeat re-read below should differ from beacon[3] above = HP actively running)"
echo "-----------------------------------------------------------------------"
