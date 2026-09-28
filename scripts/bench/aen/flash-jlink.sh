#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink.sh [--replace-atoc] [--atoc-unqueryable] <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe + the Alif SETOOLS, both Linux binaries on this
# bench). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- J-Link DIRECT MRAM flash (no SE-UART), per docs/aen-bench-bringup.md.
#
# Writes the SAME signed ATOC package SETOOLS (flow A) burns, to the SAME MRAM
# address, but over SWD via J-Link's built-in Alif MRAM loader -- which only
# activates for the part-number device profile $JLINK_DEVICE_FLASH (NOT the
# generic Cortex-M55 used for reads/RAM-run). Then a PIN reset (RSetType 2) re-runs
# the SE boot ROM, which verifies + boots the image from MRAM exactly as on a
# SETOOLS flash. No re-signing, no keys, no SE-UART maintenance-window race.
#
# This bench's examples are ITCM-load-via-ATOC (a single self-contained
# AppTocPackage.bin written as ONE blob at the package start address; the SE loads
# the embedded app to ITCM 0x58000000 and boots), so this writes that one blob --
# NOT the slot0-XIP two-blob (app@0x80010000 + ATOC) variant in the doc.
#
# Prereqs: new probe running Alif-aware firmware so `device $JLINK_DEVICE_FLASH`
# CONNECTS (the old J-Link PLUS fw 2023-01-30 failed this connect-under-reset -- the
# whole reason flow A was used here). J-Link V9.46+ has the MRAM loader built in.
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
SIZE="${2:-0x500}"
bench_require_setools || exit $?

# GUARD (alp-sdk#2027) -- resolved over $SE_UART alone, well before this
# script's own first J-Link touch (step 0 below), so the query (and any
# reset it might trigger) has settled before the probe is involved at all.
bench_flowd_atoc_guard "$REPLACE_ATOC" "$ATOC_UNQUERYABLE" flash-jlink ALP-HE || exit $?

SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns.
JLINK_ARGS=(bench_jlink_run)
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
# See ram-run.sh (issue #935): if BUF_SYM is empty, do NOT fold it into BUF --
# BUF would silently become the bare string "0x" and step 4's `mem8 $BUF,
# $SIZE` would run as `mem8 0x, $SIZE`, printing an EMPTY "RAM console" block
# indistinguishable from a boot failure. Step 4 below checks BUF_SYM directly.
BUF_SYM=$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
BUF=0x$BUF_SYM

# 0. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. Same DPIDR gate as
# flash-jlink-mramxip.sh (see that script for the full rationale): JLINK_SN
# narrows probe choice but does not itself prove which board answered. Hard
# ABORT, not a warning -- read-only connect first, no writes until confirmed.
cat > "${TMPDIR:-/tmp}/flowd-preflight.jlink" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/flowd-preflight.jlink" \
  > "${TMPDIR:-/tmp}/flowd-preflight.out" 2>&1 || true
bench_jlink_assert_aen_dpidr "${TMPDIR:-/tmp}/flowd-preflight.out" "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# 1. stage the image + the per-app signed-ATOC config (same JSON flow-run.sh uses)
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON

cd "$SET"
echo ">>> FLOW-D J-Link flash $NAME  (ram_console_buf=${BUF_SYM:-none (UART console)})" >&2
# 2. build the signed ATOC package (app-gen-toc only -- NO SE-UART) + read its
#    MRAM placement from the generated map (shifts per build/config -- never hardcode).
./app-gen-toc -f "build/config/$NAME.json" >"${TMPDIR:-/tmp}/gentoc.log" 2>&1 || { echo "gen-toc FAILED"; tail "${TMPDIR:-/tmp}/gentoc.log"; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ADDR" ] && { echo "could not parse APP Package Start Address from build/app-package-map.txt"; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ADDR" >&2

# 2b. SECTOR-PAD (alp-sdk#2233): SEGGER's built-in loader rewrites the WHOLE
#    16 KiB sector(s) $PKG touches and never reads their prior contents first,
#    so the bytes outside $PKG in its first/last sector would otherwise become
#    0xFF. Read those sectors' CURRENT MRAM content and overlay $PKG on them --
#    the padded image below is what actually gets `loadbin`ed, so the loader's
#    whole-sector rewrite reproduces the neighbours unchanged instead.
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-flash-jlink-XXXXXX")" || exit 9
bench_flowd_prepare_write flash-jlink "$FLOWD_SCRATCH" "$PKG:$ADDR" || exit 9

# 3. J-Link CommanderScript: part-number device unlocks the MRAM loader; write
#    the sector-padded image(s), then PIN reset (RSetType 2) so the SE boot ROM
#    reloads it. `verifybin` is deliberately GONE here (#2233 finding 2): it
#    only ever compared against J-Link's own in-process flash cache, never a
#    fresh chip read -- see the bench_flowd_proof step below, which replaces it.
#
# Computed into a variable BEFORE the heredoc, not `$(...)` inline inside it
# (#2233 review item 13c): a `$(...)` inside a heredoc discards the command's
# own exit status -- a failed bench_flowd_loadbin_lines would silently splice
# in NOTHING and this script would `loadbin` nothing at all, reporting success
# on a write that never happened. Refuse loudly instead.
FLOWD_LOADBIN_LINES="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)" || {
  echo "!! bench_flowd_loadbin_lines failed for $FLOWD_MANIFEST -- refusing to write nothing." >&2
  exit 9
}
[ -n "$FLOWD_LOADBIN_LINES" ] || {
  echo "!! bench_flowd_loadbin_lines produced no loadbin line for $FLOWD_MANIFEST -- refusing." >&2
  exit 9
}
# RACE CHECK setup (#2233 review major 4): the write session below savebin's
# the SAME sectors into $FLOWD_SCRATCH/prewrite BEFORE the loadbin line(s),
# so a host-side compare against the pre-read afterward can catch a write
# that landed between the pre-read and this session -- see bench-env.sh's
# "Pre-read -> write RACE detection" section for what this can and cannot
# catch.
FLOWD_PREWRITE_LINES="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/prewrite")"
FLOWD_WRITE_JLINK="${TMPDIR:-/tmp}/flowd.jlink"
FLOWD_WRITE_OUT="${TMPDIR:-/tmp}/flowd.out"
cat > "$FLOWD_WRITE_JLINK" <<EOF
si SWD
speed $JLINK_SPEED
device $DEV
connect
$FLOWD_PREWRITE_LINES
$FLOWD_LOADBIN_LINES
RSetType 2
r
g
exit
EOF

# FLOWD_DRY_RUN (#2233 review blocker 1a): exit right here, having printed
# what the write session WOULD run, WITHOUT ever invoking JLinkExe on it --
# following erase-storage.sh's own --dry-run model. This is the primary
# guarantee behind FLOWD_DRY_RUN's promise that no probe is ever opened for
# a write; bench_jlink_run() also independently refuses a loadbin/erase
# CommandFile in this mode as a backstop (bench-env.sh), but that backstop
# must never be the ONLY thing standing between this script and a real write.
if [ -n "$FLOWD_DRY_RUN" ]; then
  # alp-sdk#2233 review round 3, finding 4: "no probe was opened" was false
  # here -- the read-only DPIDR preflight above already opened one before
  # this line is ever reached. Only the WRITE session is skipped.
  echo "--- DRY RUN: nothing written, no write session was run (read-only probe access only, FLOWD_DRY_RUN) ---"
  cat "$FLOWD_WRITE_JLINK"
  exit 10
fi

# Write the transcript FIRST, fully, then grep|head it for display (#1488
# finding 5) -- a `... | tee out | grep ... | head -N` pipeline lets `head`
# exit after N lines and SIGPIPE grep, which then closes tee's stdout pipe;
# tee can die from that SIGPIPE before JLinkExe's full transcript is written
# to disk, and the connect-failure check below depends on the FULL transcript.
#
# CAPTURE THE WRITE-SESSION STATUS (alp-sdk#2233 review round 3, finding 3):
# a bare `|| true` used to swallow bench_jlink_run()'s own exit code, so its
# FLOWD_DRY_RUN backstop refusal (rc=13) fell straight through to the
# connect-failure text grep below (which finds nothing, since no session
# ever ran) and on into the race check/proof/boot as if a write had
# happened. `write_rc=0; cmd || write_rc=$?` keeps `set -e` from aborting on
# an ordinary connect failure (still handled by the text grep just below)
# while capturing the one case that matters here directly, on the same line
# -- not via a later `if ...; then ...; fi; rc=$?`, which reads the WRONG
# status when the `if` takes no branch (see bench_flowd_proof's retry-loop
# fix in bench-env.sh for the measured proof: `if false; then :; fi; echo
# $?` prints 0, not 1).
write_rc=0
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$FLOWD_WRITE_JLINK" > "$FLOWD_WRITE_OUT" 2>&1 || write_rc=$?
if [ "$write_rc" -eq 13 ]; then
  echo "!! bench_jlink_run REFUSED the write session -- FLOWD_DRY_RUN backstop (rc=13)." >&2
  echo "   Refusing before any race check, proof, or boot." >&2
  exit 13
fi
grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found" "$FLOWD_WRITE_OUT" | head -30
echo "----- (full log: $FLOWD_WRITE_OUT) -----"
if grep -qi "Could not connect to the target device" "$FLOWD_WRITE_OUT"; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe (same blocker the doc records)."
  echo "   The MRAM was NOT written. Check the new probe's firmware / connect-under-reset behaviour."
  exit 2
fi

# RUN THE PROOF BEFORE THE RACE CHECK, PRINT BOTH (alp-sdk#2233 review round
# 3, finding 7): an earlier version ran the race check FIRST and exited 11
# on a detected race before the proof ever ran, hiding whether the write
# even landed. Both now always run, both verdicts are always printed, and a
# detected race still wins the final exit code (11) even when the proof
# passed -- the race means something ELSE touched a to-be-padded sector, so
# the whole picture is suspect regardless of what the proof says.
#
# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1488 verifybin gate):
# a FRESH read-only J-Link session -- a new JLinkExe process, so nothing here
# can be served from the write session's own flash cache -- savebin's every
# padded range back and cmp's it byte-for-byte against the padded image,
# which proves both that $PKG landed AND that its sector neighbours survived
# THIS write. This is NOT a persistence proof across a power cycle -- see
# bench_flowd_proof's own header in bench-env.sh, and flash-jlink-mramxip.sh's
# "ACCEPTANCE ON THIS PATH IS A COLD-CYCLE READBACK" note for why a cold-cycle
# read remains the standing end-to-end requirement on top of this.
proof_failed=0
if ! bench_flowd_proof flash-jlink "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
  echo "!! READ-BACK PROOF FAILED -- MRAM does NOT match the padded image for $PKG @ $ADDR."
  echo "   Do not treat this board as flashed."
  proof_failed=1
fi
if [ "$proof_failed" -eq 0 ]; then
  echo "verify: read-back proof OK ($PKG @ $ADDR, sector-padded; a fresh-session read, NOT a cold-cycle persistence proof)"
fi

# RACE CHECK (#2233 review major 4): compare this session's prewrite savebin
# against the ORIGINAL pre-read, on the host. Any difference means something
# else wrote to a touched sector between the pre-read and this session's own
# pre-load savebin -- the padded image (built from the stale pre-read) may
# have just overwritten it. Both copies are kept; nothing here is deleted.
# Also fails closed (alp-sdk#2233 review round 3, finding 7) if the write
# session's OWN prewrite savebin reported a read failure -- see
# bench_flowd_check_race's own header in bench-env.sh.
race_failed=0
if ! bench_flowd_check_race flash-jlink "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/sectors" "$FLOWD_SCRATCH/prewrite" "$FLOWD_WRITE_OUT"; then
  echo "!! RACE DETECTED -- restore from $FLOWD_SCRATCH/sectors and $FLOWD_SCRATCH/prewrite" >&2
  echo "   before trusting this board. The board HAS already been written and booted" >&2
  echo "   (RSetType 2/r/g already ran in the same session) -- this is a post-hoc warning," >&2
  echo "   not a block on the write." >&2
  race_failed=1
fi

if [ "$race_failed" -eq 1 ]; then
  exit 11
fi
if [ "$proof_failed" -eq 1 ]; then
  exit 3
fi

# 4. SES has re-booted the app; attach read-only with the GENERIC device and dump
#    the RAM console (the part-number profile can't re-halt the running secure core).
sleep 3
if [ -z "$BUF_SYM" ]; then
  echo "----- $NAME RAM console: no 'ram_console_buf' in this image (UART-console app) -----" >&2
  echo "      the flash above still completed -- this is not a boot failure. Read the" >&2
  echo "      console via the labgrid 'console' resource instead." >&2
else
  cat > "${TMPDIR:-/tmp}/flowd-read.jlink" <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
  "${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/flowd-read.jlink" 2>"${TMPDIR:-/tmp}/flowd-rd.err" > "${TMPDIR:-/tmp}/flowd-rd.out" || true
  # JLinkExe exits 0 even when it never opened the probe, so `|| true` above
  # hides a total connect failure and the decode below would render it as
  # empty target output (alp-sdk#1318).
  bench_jlink_assert_connected "${TMPDIR:-/tmp}/flowd-rd.out" "Flow D read-back" || exit 7
  echo "----- $NAME RAM console (flow-D flashed, SE-booted) -----"
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' "${TMPDIR:-/tmp}/flowd-rd.out"
  echo; echo "--------------------------------------------------------"
fi
