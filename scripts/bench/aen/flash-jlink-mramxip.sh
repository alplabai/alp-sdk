#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-mramxip.sh [--replace-atoc] [--atoc-unqueryable] <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe + the Alif SETOOLS, both Linux binaries on this
# bench). Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# FLOW D -- MRAM-XIP / slot0 TWO-BLOB variant (per docs/aen-bench-bringup.md §Flow D).
#
# NOT the only way to provision this shape: bench-proven 2026-07-19, a plain
# `west flash` / `app-write-mram -p` over the SE-UART (Flow A, the `alif_flash`
# runner) burns the same two blobs in one pass -- the runner auto-detects the
# shape from the app's own reset vector. This script is the faster SWD-only
# alternative (no SE-UART reset race), not a capability requirement.
#
# Unlike flash-jlink.sh (single-blob ITCM-load-via-ATOC, loadAddress 0x58000000),
# this is for an app LINKED INTO MRAM slot0 (@0x80010000, CONFIG_FLASH_LOAD_OFFSET
# 0x10000) that overflows ITCM -- e.g. a real NPU model. Two blobs are written:
#   1. zephyr.bin        -> 0x80010000  (the slot0-linked app)
#   2. AppTocPackage.bin -> <parsed>    (the signed ATOC the SE boots)
# The app entry uses mramAddress 0x80010000 (the FULL address -- the 0x10000
# OFFSET gives SETOOLS "Invalid Global Address") + flags ["boot"] (NOT loadAddress),
# so app-gen-toc signs the app where it sits in MRAM rather than embedding it.
#
# The image MUST link at the slot0 offset (reset vector 0x8001xxxx). Since
# alp-sdk#1067 the board _defconfig supplies that (CONFIG_USE_DT_CODE_PARTITION=y),
# so a plain build is already correct; a 0x8000xxxx vector now means something in
# the build OVERRODE it (a Flow C fragment/overlay left layered on), not that the
# app forgot to opt in. FLASH_LOAD_OFFSET back at 0 links the image at the MRAM
# base and it faults on an SE slot0 boot.
#
# GOTCHA -- returning to ITCM apps: once a slot0 image is resident, the SE boots
# it preferentially over an ITCM-load ATOC, and a J-Link `erase` does NOT clear
# MRAM. To flash ITCM-load (flow-C/flash-jlink.sh) apps afterwards, erase slot0
# first over the SE-UART:  app-write-mram -c $SE_UART -e "0x80010000 0x60000".
#
# HE-ONLY: this script hard-codes cpu_id M55_HE / APP_ADDR 0x80010000 (the HE
# slot0 window, see scripts/aen_atoc.py SLOT0_WINDOWS). There is no HP
# MRAM-XIP flow today -- every HP image is ITCM-loaded (loadAddress
# 0x50000000, see flash-jlink-hp.sh), so an HP-linked binary here is a
# script-selection mistake, not a supported shape; step 0's reset-vector
# check below diagnoses that case explicitly instead of guessing "itcm
# overlay".
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
# THIS SCRIPT SPECIFICALLY -- an open question, still UNVERIFIED ON SILICON
# because this bench cannot be reached to test it: does
# `maintenance -c $SE_UART -opt gettoc` itself reset the target? If it does,
# AP[3] (APAddr 0x00300000) disappears (see the #1902 note below on this
# script's pre-programming `h` gate) between the guard's query and step 3's
# halt-before-programming check. The guard call sits right after the
# reset-vector sanity check (below, "0. SANITY") -- which needs only the
# local .bin and no probe -- and still well before this script's own first
# J-Link touch (step 0b) or either `loadbin`, specifically so any reset the
# query might trigger has the most possible real time (SETOOLS invocation,
# staging, app-gen-toc) to complete its reboot before step 3 tries to halt --
# but that is a mitigation, not a proof. If it is NOT enough and the
# interaction is real, the documented failure mode is the existing
# "!! HALT FAILED before programming" abort (exit 5, below): NOTHING is
# written and slot0 keeps its previous contents -- a safe, explicit abort,
# not a silent one and not MRAM corruption. Exit 5 is NOT by itself the
# signal that this open question is real, though: bench_flowd_atoc_guard's
# OWN abort (an unverified or foreign-entry resident-ATOC query) also
# returns 5, propagated straight through from below -- with $SE_UART
# exported, both causes report exit 5. Distinguish them by the stderr text
# ("!! ABORT (flash-jlink-mramxip): could not read the resident ATOC" /
# "this write REPLACES" for the guard, vs "!! HALT FAILED before
# programming" for the halt gate), not by the exit code alone.
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
SIZE="${2:-0x800}"
bench_require_setools || exit $?

SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns, instead of the old
# JLINK_SN-only selection which could not distinguish same-serial probes.
# The DPIDR gate below (step 0b) is a separate, additional check for which
# CHIP answered -- keep both.
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
APP_ADDR=0x80010000                 # MRAM base 0x80000000 + slot0 offset 0x10000
# No 0x$BUF_SYM fallback here if BUF_SYM is empty: BUF would silently become
# the bare string "0x", and step 4's `mem8 $BUF, $SIZE` would run as
# `mem8 0x, $SIZE` -- a malformed address that reads back nothing and prints
# an EMPTY "RAM console" block indistinguishable from a boot failure. See
# ram-run.sh (issue #935) for the same guard. Step 4 below checks BUF_SYM
# directly and skips the dump instead.
BUF_SYM=$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
BUF=0x$BUF_SYM

# 0. SANITY: the image MUST be slot0-linked (reset-vector word reads 0x8001xxxx).
RV=$(xxd -e -l 8 "$BIN" | awk '{print $3}')   # 2nd LE word = reset vector
echo ">>> FLOW-D MRAM-XIP $NAME  (reset vector=0x$RV  ram_console_buf=${BUF_SYM:-none (UART console)})" >&2
case "$RV" in
  8001*) : ;;  # good -- linked into slot0 (0x80010000 + reset-handler offset)
  8000*) echo "!! reset vector 0x$RV is BASE-linked (0x8000xxxx), not slot0."
         echo "   The board _defconfig sets CONFIG_USE_DT_CODE_PARTITION=y, so"
         echo "   something overrode it -- drop any Flow C fragment/overlay"
         echo "   (aen-flowc-itcm.conf / .overlay) and rebuild pristine."
         exit 3 ;;
  802b*) echo "!! reset vector 0x$RV is HP-slot0-linked (0x802bxxxx), not HE."
         echo "   flash-jlink-mramxip.sh is HE-only (cpu_id M55_HE, MRAM"
         echo "   window 0x80010000..0x802b0000); there is no HP MRAM-XIP"
         echo "   flow -- HP images are ITCM-loaded, use flash-jlink-hp.sh."
         exit 3 ;;
  *) echo "!! reset vector 0x$RV unexpected -- not a 0x8001xxxx slot0 image."
     echo "   Drop any &itcm overlay; let the board default link into MRAM slot0."
     exit 3 ;;
esac

# GUARD (alp-sdk#2027) -- see the open-question note above for why this sits
# as early as possible: right after the reset-vector sanity check above
# (which needs only the local `.bin`, no probe and no SETOOLS query, and
# rejects a base-linked/HP-linked image with exit 3 for free) and still well
# before step 0b's first J-Link touch below. Review MINOR 4: putting the
# guard BEFORE that sanity check meant a bad build (or a `--help` typo) paid
# for two real SE-UART round-trips -- `maintenance -opt getbanner` then
# `-opt gettoc` -- and risked whatever the still-open gettoc-reset question
# above implies, for a failure this script could already detect for free
# from the local file alone.
bench_flowd_atoc_guard "$REPLACE_ATOC" "$ATOC_UNQUERYABLE" flash-jlink-mramxip ALP-HE || exit $?

# 0b. SAFETY GATE -- confirm we are talking to the AEN E8, not some other probe
# on the bench, BEFORE any MRAM write. The AEN E8 SW-DP IDR is 0x4C013477
# (BENCH-VERIFIED, see docs/bring-up-aen.md); GD32_DPIDR (0x0BE12477,
# exported by bench-env.sh) is the only GD32 candidate on record but has
# NOT been measured on a GD32 with a probe attached (see #1369) -- treat
# it as unattested, not bench-verified. Flashing the wrong board is the one
# unrecoverable bench mistake, so this is a hard ABORT, not a warning --
# read-only connect first, no writes happen until the ID is confirmed.
# `${VAR:-default}`, NOT a bare assignment: bench-env.sh (sourced above) is
# the documented single source and declares these overridable
# (`export AEN_DPIDR="${AEN_DPIDR:-4C013477}"`), which its own header states:
# "Override any of them by exporting the variable before invoking a helper".
# A bare assignment here silently discarded that export and then aborted
# against the value the operator had explicitly overridden -- and this was the
# ONLY one of the six callers of bench_jlink_assert_aen_dpidr that did so
# (#1497).  V2N_CM33_DPIDR was never re-declared here either, so the file used
# one source for two IDs and another for the third.
#
# The declarations stay (#1527: deleting the pair to silence shellcheck
# weakened the wrong-board MRAM-write gate during #1488 and was reverted) --
# they are now defaulting rather than overriding, which satisfies both.
AEN_DPIDR="${AEN_DPIDR:-4C013477}"
# GD32_DPIDR IS read -- just not in THIS file. bench_jlink_assert_aen_dpidr,
# defined in the sourced bench-env.sh (line ~165, `grep -qi "$GD32_DPIDR"
# "$out"`), reads it to name the wrong board. Plain shellcheck can't see a
# cross-file use like that, which is why CI and stage_shellcheck both
# invoke shellcheck with -x (follow `source`) here -- with -x this line
# correctly reports no SC2034. Kept as the documented wrong-board value
# alongside AEN_DPIDR's pin -- #1527: do NOT delete this pair to silence
# the linter, that weakened the wrong-board MRAM-write gate during #1488
# and had to be reverted.
GD32_DPIDR="${GD32_DPIDR:-0BE12477}"
cat > "${TMPDIR:-/tmp}/flowd-mramxip-preflight.jlink" <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
bench_jlink_run -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/flowd-mramxip-preflight.jlink" \
  > "${TMPDIR:-/tmp}/flowd-mramxip-preflight.out" 2>&1 || true
bench_jlink_assert_aen_dpidr "${TMPDIR:-/tmp}/flowd-mramxip-preflight.out" "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# #1902 -- THE CUSTOM PATCHED-FLM PATH DOES NOT PRODUCE TRUSTWORTHY BYTES.
# This warns rather than refuses: on a J-Link V11+ probe that can select SEGGER's
# built-in AE822FA0E5597LS0_M55_HE profile, this script is fine and is the
# documented fast path.  The custom AE822_ALP_M55_HE device -- generic Cortex-M55
# connect + a hand-patched copy of Alif's Ensemble.FLM -- is the one that fails.
if [ "$DEV" = "AE822_ALP_M55_HE" ]; then
  echo "?? WARNING: JLINK_DEVICE_FLASH=$DEV is the hand-patched-FLM workaround." >&2
  echo "   Bench-measured 2026-09-04: only 1 of 4 flashes on this path survived a" >&2
  echo "   cold power-cycle byte-exact.  Failures corrupt slot0 in a specific way --" >&2
  echo "   of the 128-bit MRAM ECC words that differ, the LAST 32-bit word is wrong" >&2
  echo "   in 107/107 and 66/66 (a torn 128-bit commit; NOT a sector or page fault," >&2
  echo "   0 of 127 cluster starts are 0x400-aligned).  A corrupted image then fails" >&2
  echo "   SES verification (cert_verify_wrapper returned 0xF1000009, TOC flags 'u s'" >&2
  echo "   not 'u VB') and never boots." >&2
  echo "   It cannot be fixed by quiescing the core first: AP[3] (APAddr 0x00300000)," >&2
  echo "   the AHB-AP carrying the M55 debug domain, exists ONLY while the SES has" >&2
  echo "   booted a slot0 image.  Erase slot0 and this profile cannot connect at all" >&2
  echo "   (0 of 5 attempts reached programming).  Attachable and quiesced are" >&2
  echo "   mutually exclusive on this part." >&2
  echo "   FIXED 2026-09-05 -- USE THE BUILT-IN PROFILE INSTEAD:" >&2
  echo "     JLINK_DEVICE_FLASH=AE822FA0E5597LS0_M55_HE" >&2
  echo "   on a probe whose firmware reports capability 0x52.  Bench-proven on" >&2
  echo "   J-Link 603000869 (Hardware version V13.00, firmware V13 compiled" >&2
  echo "   Aug 26 2026): 3 of 3 flashes cold-cycle byte-exact with flags 'u VB'," >&2
  echo "   including aen-wdt-feed twice -- the app THIS path corrupted 2 of 2." >&2
  echo "   Flow A (scripts/bench/aen/flash-run.sh) also remains correct." >&2
  echo "   ALWAYS confirm any write on this path with a cold-cycle readback." >&2
fi

# 1. stage the app + write the slot0 (mramAddress) signed-ATOC config.
#
# HAZARD (found 2026-09-05): this cp OVERWRITES the staged image of the same
# name, and for the canonical restore that matters.  The staged
# aen-npu-inference-person-mram.bin is 314784 B (md5
# 5839e003d5d069f6fd912eb22774d037), while the in-tree build dir currently holds
# a DIFFERENT 314772 B binary -- so "just re-run this script on the person_detect
# build dir" silently replaces the canonical staged image with another one, and
# the thing you restore is not the thing you think.  To restore canonical slot0,
# run app-gen-toc + app-write-mram against the STAGED .bin directly instead of
# re-staging from a build dir, and prove it with a cold-cycle savebin diff.
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME-slot0.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"] }
}
JSON

# #1069 window/overlap guard -- see scripts/aen_atoc.py. This config is
# always the fixed HE/0x80010000 entry above (this script is HE-only, see
# header), so the guard is a no-op today; it's here so this call site
# can't silently drift from the shared check if the config ever changes.
python3 "$ALP_SDK_DIR/scripts/aen_atoc.py" "$SET/build/config/$NAME-slot0.json" || exit 1

cd "$SET"
# 2. build the signed ATOC (app-gen-toc only) + read the ATOC MRAM placement.
./app-gen-toc -f "build/config/$NAME-slot0.json" >"${TMPDIR:-/tmp}/gentoc-mramxip.log" 2>&1 \
  || { echo "gen-toc FAILED"; tail -20 "${TMPDIR:-/tmp}/gentoc-mramxip.log"; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }
echo "    app  -> $APP_ADDR ($(stat -c%s "$SET/build/images/$NAME.bin") B)" >&2
echo "    atoc -> $ATOC_ADDR ($(stat -c%s "$PKG") B)" >&2

# SECTOR-PAD (alp-sdk#2233): the built-in loader rewrites the WHOLE 16 KiB
# sector(s) EITHER blob touches and never reads their prior contents first --
# see bench-env.sh's Flow D section header. Read those sectors' current MRAM
# content and overlay both blobs on them (one shared read/build pass handles
# both writes, and merges them into one image if they ever land in the same
# sector); the padded images are what actually get loadbin'ed below.
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-mramxip-XXXXXX")" || exit 9
bench_flowd_prepare_write flash-jlink-mramxip "$FLOWD_SCRATCH" \
	"$SET/build/images/$NAME.bin:$APP_ADDR" "$PKG:$ATOC_ADDR" || exit 9

# 3. J-Link: part-number device unlocks the MRAM loader; write BOTH padded blobs,
#    sanity-check the reset vector, then PIN reset (RSetType 2) -> SE boot ROM boots it.
# `, noreset` on BOTH loadbins is load-bearing -- see #1902.  By default
# `loadbin` does an "implicit reset & halt of MCU", which on the E8 is an
# AIRCR.SYSRESETREQ that resets the WHOLE system including the Secure Enclave.
# The SES then re-boots slot0, so the M55 starts executing XIP out of the very
# MRAM J-Link is mid-way through erasing and programming.  Bench log evidence
# (2026-09-04, aen-qenc-readout): the second loadbin's own reset reported
#   Reset: ARMv8M core with Security Extension enabled detected. Switch to secure domain.
# and then died with the app demonstrably running out of slot0 --
#   ****** Error: PC of target system has unexpected value after preparing target. (PC = 0x8001D38E)!
#   Failed to perform RAMCode-sided Prepare()
# -- while the first loadbin, whose reset could NOT switch to the secure domain
# ("switching to secure domain is not possible"), had already reported
#   ****** Error: Verification failed @ address 0x80010000
# This is a RACE between the SES re-booting slot0 and J-Link's program/verify,
# which is why the SAME app passed and failed under identical settings and why
# a busy resident app (aen-wdt-feed feeding a watchdog, aen-sdcard-readout --
# renamed aen-sdhc-probe, #2051, and no longer doing any I/O at all now that
# sdhc0 is disabled on this board -- doing long I/O at the time) failed far
# more often than one that idles quickly.  It is NOT flaky
# MRAM and NOT a probe-firmware limit.  `noreset` keeps the single explicit
# reset+halt below as the only reset in the sequence, so nothing is executing
# from MRAM while MRAM is being written.
#
# NO RESET BEFORE PROGRAMMING -- `h` alone, on the live core.
# Bench-measured 2026-09-04, 8 back-to-back probe-only sessions whose logs are
# BYTE-IDENTICAL apart from a 1 mV probe ADC reading (`VTref=1.820V` vs
# `VTref=1.819V`) -- so this is not timing.  The AHB-AP carrying the M55 debug
# domain, AP[3] (APAddr 0x00300000): AHB-AP (IDR: 0x34770008), is present in the
# PRE-reset scan 8/8 (`AP[3]: Core found`, `CPUID register: 0x411FD220`,
# `Found Cortex-M55 r1p0, Little endian.`) and ABSENT from every post-reset scan,
# which stops at AP[2] (APAddr 0x00070000): AXI-AP (IDR: 0x34770017) and then
# `Could not find core in Coresight setup`.  So `RSetType 2; r` was DESTROYING the
# debug access the halt needs: 0/8 halts after a reset, versus a core reachable on
# a plain `connect` 8/8 here and 15/15 across the earlier flashing logs.
# Attaching to the live core and halting it took programming from 6/12 to 12/12.
# The reset that BOOTS the new image still happens, AFTER both blobs are written.
#
# !! ACCEPTANCE ON THIS PATH IS A COLD-CYCLE READBACK, NOT `verifybin`.  Halting a
# !! long-running app and programming from that state has a 2026-07-09 bench
# !! precedent of a SILENT NON-PERSIST -- `Verify successful.` followed by a cold
# !! power-cycle REVERTING slot0.  This probe also warns its firmware
# !! "does not handle I/D-cache correctly" on ARMv8-M, and an app that left
# !! D-cache on is exactly that case.  Prove a flash by cutting power at the
# !! DPS-150 and re-reading slot0.  RESOLVED 2026-09-04: the AEN place now drives
# !! DPS-150 10A2617F4486 at the documented 16.0 V (pwr-only.yaml re-cabled, the
# !! retired V2N bench place's agent disabled), so
# !!   LG_ENV=~/board-farm/labgrid/pwr-only.yaml labgrid-client -p <your-bench-place> power off|on
# !! really cuts power and the cold-cycle proof is available on this bench.
#
# SetSkipProgOnCRCMatch = 0: never let J-Link decide a page is already correct
# from a debug READ.  Debug-AP reads of this part are documented to lie in some
# states (see reference_aen_e8_bench_traps), and trusting one here would silently
# skip programming a page that does not actually match.
#
# `verifybin` is deliberately GONE here (#2233): it only ever compared against
# J-Link's own in-process flash cache, never a fresh chip read -- see the
# bench_flowd_proof gate below, which replaces both lines. Both loadbin lines
# now come from bench_flowd_loadbin_lines against the SECTOR-PADDED images
# built above, with `, noreset` on both -- unchanged from before (#1902,
# see the header comment above this block): still the only reset in the
# sequence, still after both blobs are written.
#
# Computed into a variable BEFORE the heredoc (#2233 review item 13c): a
# `$(...)` inline inside a heredoc discards the command's own exit status.
FLOWD_LOADBIN_LINES="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 1)" || {
  echo "!! bench_flowd_loadbin_lines failed for $FLOWD_MANIFEST -- refusing to write nothing." >&2
  exit 9
}
[ -n "$FLOWD_LOADBIN_LINES" ] || {
  echo "!! bench_flowd_loadbin_lines produced no loadbin line for $FLOWD_MANIFEST -- refusing." >&2
  exit 9
}
# RACE CHECK setup (#2233 review major 4) -- savebin's the same sectors right
# after `h` (the core is already halted here) and BEFORE the load lines --
# see bench-env.sh's "Pre-read -> write RACE detection" section.
FLOWD_PREWRITE_LINES="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/prewrite")"
FLOWD_WRITE_JLINK="${TMPDIR:-/tmp}/flowd-mramxip.jlink"
FLOWD_WRITE_OUT="${TMPDIR:-/tmp}/flowd-mramxip.out"
cat > "$FLOWD_WRITE_JLINK" <<EOF
si SWD
speed $JLINK_SPEED
device $DEV
connect
exec SetSkipProgOnCRCMatch = 0
h
$FLOWD_PREWRITE_LINES
$FLOWD_LOADBIN_LINES
mem32 $APP_ADDR 2
RSetType 2
r
g
exit
EOF

# FLOWD_DRY_RUN (#2233 review blocker 1a): exit right here, having printed
# what the write session WOULD run, WITHOUT ever invoking JLinkExe on it.
if [ -n "$FLOWD_DRY_RUN" ]; then
  # alp-sdk#2233 review round 3, finding 4: "no probe was opened" was false
  # -- the read-only DPIDR preflight above already opened one.
  echo "--- DRY RUN: nothing written, no write session was run (read-only probe access only, FLOWD_DRY_RUN) ---"
  cat "$FLOWD_WRITE_JLINK"
  exit 10
fi

# Write the transcript FIRST, fully, then grep it for display (#2233 review
# item 12 -- the SAME SIGPIPE hazard #1488 finding 5 fixed in flash-jlink.sh:
# a `... | tee out | grep ... | head -N` pipeline lets `head` exit early and
# SIGPIPE grep, which can close tee's stdout pipe and kill JLinkExe mid-write
# -- and this script's downstream halt/loadbin-failure checks below all read
# from the same file, so a truncated transcript would misreport them too).
#
# CAPTURE THE WRITE-SESSION STATUS (alp-sdk#2233 review round 3, finding 3) --
# see flash-jlink.sh's identical comment for why `write_rc=0; cmd ||
# write_rc=$?`, not a later `if ...; then ...; fi; rc=$?`.
write_rc=0
bench_jlink_run -nogui 1 -CommanderScript "$FLOWD_WRITE_JLINK" > "$FLOWD_WRITE_OUT" 2>&1 || write_rc=$?
if [ "$write_rc" -eq 13 ]; then
  echo "!! bench_jlink_run REFUSED the write session -- FLOWD_DRY_RUN backstop (rc=13)." >&2
  echo "   Refusing before any halt/loadbin check, race check, proof, or boot." >&2
  exit 13
fi
grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found|= " "$FLOWD_WRITE_OUT" | head -40
echo "----- (full log: $FLOWD_WRITE_OUT) -----"
if grep -qi "Could not connect to the target device" "$FLOWD_WRITE_OUT"; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe."; exit 2
fi

# #1902 -- CONFIRM THE HALT, SCOPED TO BEFORE PROGRAMMING.
#
# SCOPE IS LOAD-BEARING.  The halt that matters is the one BEFORE the first
# `loadbin`.  The trailing `RSetType 2; r; g` that BOOTS the image also tries to
# halt and on this part ALWAYS fails, because AP[3] (APAddr 0x00300000) is gone
# after any reset (see above).  That trailing failure is EXPECTED and harmless:
# the bytes are written and verified by then and the SES boots the image anyway.
#
# An earlier version of this gate grepped the WHOLE log, tripped on that trailing
# reset, and failed 12 of 12 runs whose programming had actually SUCCEEDED
# (`Program: 2.272s`, both blobs, 2/2 `Verify successful.`), printing
# "NOTHING was written" over a log showing the opposite.  Never widen it back.
#
# Split at the first `Downloading file` -- J-Link prints it when loadbin starts.
FLOWD_WRITE_PREPROG="${TMPDIR:-/tmp}/flowd-mramxip.preprog"
awk '/Downloading file/{exit} {print}' "$FLOWD_WRITE_OUT" > "$FLOWD_WRITE_PREPROG"

# Confirm the halt POSITIVELY: `h` prints this register dump ONLY when the core
# actually halted (on failure it prints `WARNING: CPU could not be halted` and no
# PC line).  Do NOT gate on `Cortex-M55 identified.` -- that is printed at every
# connect while the app is still running.
if ! grep -qE "^PC = [0-9A-F]{8}, CycleCnt = " "$FLOWD_WRITE_PREPROG"; then
  echo "!! HALT FAILED before programming -- the core was RUNNING for the flash."
  grep -iE "Could not find core in Coresight setup|CPU could not be halted|Failed to halt CPU|Failed to preserve target RAM" "$FLOWD_WRITE_PREPROG" | head -5
  echo "   With 'loadbin ..., noreset' there is no fallback reset, so the RAMCode"
  echo "   workspace at 0x00000000-0x0001FFFF could not be preserved and both blobs"
  echo "   were skipped.  NOTHING was written -- slot0 still holds its previous"
  echo "   contents.  This is NOT MRAM corruption."
  exit 5
fi
echo "halt: core halted before programming ($(grep -oE '^PC = [0-9A-F]{8}' "$FLOWD_WRITE_PREPROG" | head -1))"

# #1902 -- A FAILED `loadbin` IS NOT SURVIVABLE, AND (AT THE TIME) `verifybin`
# DID NOT CATCH IT.
#
# Bench-measured 2026-09-04: a run whose ATOC `loadbin` died with
#   ****** Error: Timeout while preparing target, core does not stop. (PC = 0x80015190, XPSR = 0x01000003, SP = 0x20002EA8)!
#   Failed to perform RAMCode-sided Prepare()
#   Unspecified error -1
# still reported `verify: 2/2` and exited 0.  It passed because the PREVIOUS
# app's ATOC was still resident and happened to satisfy the comparison -- the
# resident ATOC reported size 78932 (aen-gpu2d-bench's) while the app being
# flashed was aen-wdt-feed at 78752.  So counting `verifybin` successes was NOT
# sufficient: a stale-but-self-consistent blob verified clean. #2233's
# bench_flowd_proof gate below cannot repeat this specific failure -- it reads
# through a FRESH JLinkExe process (no flash cache) and compares against a
# manifest built from THIS run's own blobs, so a stale resident blob would
# read back as ITSELF, not as a match -- but this check stays as a fast,
# explicit fail on a known-bad transcript pattern regardless.
#
# Fail on the loadbin error directly.  Kept BEFORE the read-back proof below,
# so a loadbin that already died gets this specific, more informative message
# instead of the proof's generic mismatch report.
if grep -qiE "Failed to perform RAMCode-sided Prepare\(\)|Timeout while preparing target|Failed to prepare for programming|Unspecified error -1" "$FLOWD_WRITE_OUT"; then
  echo "!! LOADBIN FAILED -- at least one blob was NOT written to MRAM."
  grep -iE "Timeout while preparing target|Failed to perform RAMCode-sided Prepare|Failed to prepare for programming|Unspecified error -1" "$FLOWD_WRITE_OUT" | head -4
  echo "   slot0 and/or the ATOC are now MIXED -- erase and reflash before using this board."
  exit 6
fi

# RUN THE PROOF BEFORE THE RACE CHECK, PRINT BOTH (alp-sdk#2233 review round
# 3, finding 7) -- see flash-jlink.sh's identical comment. (The halt-failure
# and loadbin-failure checks above stay BEFORE both: if nothing was written
# at all, race/proof have nothing useful to say.)
#
# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1343/#1902 verifybin
# count above) -- a FRESH read-only J-Link session savebin's both padded
# ranges back and cmp's each byte-for-byte against its padded image, proving
# both blobs landed AND that their sector neighbours survived THIS write --
# NOT a persistence proof across a power cycle. Unlike the old verifybin
# count, this reads a NEW JLinkExe process's view of MRAM, not a debug-AP
# read inside the same session the loadbins ran in -- so the #1902
# "stale-but-self-consistent blob verifies clean" failure mode above cannot
# recur here either: a stale resident blob would read back as ITSELF, which
# is not what the padded manifest (built from this run's own blobs) expects.
# The cold-cycle read-back this section's own header already requires
# ("ACCEPTANCE ON THIS PATH IS A COLD-CYCLE READBACK") remains the standing
# end-to-end requirement -- this proof is a strong, but same-power-cycle,
# addition on top of it, not a replacement for it.
proof_failed=0
if ! bench_flowd_proof flash-jlink-mramxip "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
  echo "!! READ-BACK PROOF FAILED -- MRAM does NOT match the padded images."
  echo "   slot0 content is NOT what you built.  Do not treat this board as flashed."
  proof_failed=1
fi
if [ "$proof_failed" -eq 0 ]; then
  echo "verify: read-back proof OK (app image + AppTocPackage, sector-padded; still requires a cold-cycle read to prove persistence)"
  if grep -qi "CPU could not be halted" "$FLOWD_WRITE_OUT"; then
    echo "note: the trailing boot reset could not halt the core -- EXPECTED on this part"
    echo "      (AP[3] (APAddr 0x00300000) is absent after any reset).  The image is"
    echo "      already written and verified; the SES boots it regardless."
  fi
fi

# RACE CHECK (#2233 review major 4): compare the prewrite savebin (captured
# right after `h`, before either loadbin, in THIS session) against the
# original pre-read, on the host.
race_failed=0
if ! bench_flowd_check_race flash-jlink-mramxip "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/sectors" "$FLOWD_SCRATCH/prewrite" "$FLOWD_WRITE_OUT"; then
  echo "!! RACE DETECTED -- restore from $FLOWD_SCRATCH/sectors and $FLOWD_SCRATCH/prewrite" >&2
  echo "   before trusting this board. The board HAS already been written and booted." >&2
  race_failed=1
fi

if [ "$race_failed" -eq 1 ]; then
  exit 11
fi
if [ "$proof_failed" -eq 1 ]; then
  exit 3
fi

# 4. SES has re-booted the app; attach read-only (generic device) + dump RAM console.
sleep 3
if [ -z "$BUF_SYM" ]; then
  echo "----- $NAME RAM console: no 'ram_console_buf' in this image (UART-console app) -----" >&2
  echo "      the flash above still completed -- this is not a boot failure. Read the" >&2
  echo "      console via the labgrid 'console' resource instead." >&2
else
  cat > "${TMPDIR:-/tmp}/flowd-mramxip-read.jlink" <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
  bench_jlink_run -nogui 1 -CommanderScript "${TMPDIR:-/tmp}/flowd-mramxip-read.jlink" 2>"${TMPDIR:-/tmp}/flowd-mramxip-rd.err" > "${TMPDIR:-/tmp}/flowd-mramxip-rd.out" || true
  # JLinkExe exits 0 even when it never opened the probe, so `|| true` above
  # hides a total connect failure and the decode below would render it as
  # empty target output (alp-sdk#1318).
  bench_jlink_assert_connected "${TMPDIR:-/tmp}/flowd-mramxip-rd.out" "Flow D mramxip read-back" || exit 7
  echo "----- $NAME RAM console (flow-D MRAM-XIP flashed, SE-booted) -----"
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' "${TMPDIR:-/tmp}/flowd-mramxip-rd.out"
  echo; echo "--------------------------------------------------------"
fi
