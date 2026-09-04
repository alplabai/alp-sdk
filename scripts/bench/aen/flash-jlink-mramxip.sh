#!/usr/bin/env bash
# scripts/bench/aen/flash-jlink-mramxip.sh <build-dir> [post_boot_read_bytes_hex]
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

BD="$1"
SIZE="${2:-0x800}"
bench_require_setools || exit $?
SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
DEV="$JLINK_DEVICE_FLASH"
# Select the AEN J-Link by serial: the bench has TWO J-Links (AEN + the CC3501E
# XDS110/V2N), so without SelectEmuBySN JLinkExe picks arbitrarily and "Cannot
# connect to the probe". NO hardcoded serial default here: a bench-wide serial
# (e.g. 603000869) is SHARED by two probes that differ only by USB path, and a
# silent default can pick the WRONG board (the V2N-M1 GD32, not the AEN E8).
# Export JLINK_SN yourself if you need to disambiguate by serial -- either way,
# the DPIDR gate below (step 0b), not the serial, is what stops a write to the
# wrong target.
SEL="${JLINK_SN:+SelectEmuBySN $JLINK_SN}"
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
cat > /tmp/flowd-mramxip-preflight.jlink <<EOF
$SEL
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip-preflight.jlink \
  > /tmp/flowd-mramxip-preflight.out 2>&1 || true
bench_jlink_assert_aen_dpidr /tmp/flowd-mramxip-preflight.out "MRAM write preflight" || exit 4
echo ">>> DPIDR gate OK: probe confirmed AEN E8 (0x$AEN_DPIDR)" >&2

# 1. stage the app + write the slot0 (mramAddress) signed-ATOC config.
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
./app-gen-toc -f "build/config/$NAME-slot0.json" >/tmp/gentoc-mramxip.log 2>&1 \
  || { echo "gen-toc FAILED"; tail -20 /tmp/gentoc-mramxip.log; exit 1; }
PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address"; exit 1; }
echo "    app  -> $APP_ADDR ($(stat -c%s "$SET/build/images/$NAME.bin") B)" >&2
echo "    atoc -> $ATOC_ADDR ($(stat -c%s "$PKG") B)" >&2

# 3. J-Link: part-number device unlocks the MRAM loader; write BOTH blobs, verify,
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
# a busy resident app (aen-wdt-feed feeding a watchdog, aen-sdcard-readout doing
# long I/O) failed far more often than one that idles quickly.  It is NOT flaky
# MRAM and NOT a probe-firmware limit.  `noreset` keeps the single explicit
# reset+halt below as the only reset in the sequence, so nothing is executing
# from MRAM while MRAM is being written.
#
# SetSkipProgOnCRCMatch = 0: never let J-Link decide a page is already correct
# from a debug READ.  Debug-AP reads of this part are documented to lie in some
# states (see reference_aen_e8_bench_traps), and trusting one here would silently
# skip programming a page that does not actually match.
cat > /tmp/flowd-mramxip.jlink <<EOF
$SEL
si SWD
speed $JLINK_SPEED
device $DEV
connect
exec SetSkipProgOnCRCMatch = 0
RSetType 2
r
h
loadbin $SET/build/images/$NAME.bin $APP_ADDR, noreset
loadbin $PKG $ATOC_ADDR, noreset
verifybin $SET/build/images/$NAME.bin $APP_ADDR
verifybin $PKG $ATOC_ADDR
mem32 $APP_ADDR 2
RSetType 2
r
g
exit
EOF
$JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip.jlink 2>&1 | tee /tmp/flowd-mramxip.out | \
  grep -iE "could not connect|fail|error|Verify|O\.K\.|Writing|Programming|Reset|Cortex|Found|= " | head -40
echo "----- (full log: /tmp/flowd-mramxip.out) -----"
if grep -qi "Could not connect to the target device" /tmp/flowd-mramxip.out; then
  echo "!! $DEV profile FAILED to connect -- flow D not unlocked on this probe."; exit 2
fi

# GATE ON THE VERIFY RESULT (#1343).  The two `verifybin` lines above have always
# been issued, but until now NOTHING read their outcome: the JLinkExe output goes
# to a display-only pipe, and the ONLY condition that could fail this script was
# the connect check above.  So a `Verify failed.` scrolled past and the script
# exited 0 -- reporting a good flash for bytes that never landed, which is exactly
# the failure #1343 measured (a `loadbin` reporting `O.K.` having silently skipped).
#
# That is worse than having no verify at all: anyone reading this script saw
# `verifybin` and reasonably concluded writes were checked.  The absence would at
# least have been visible.
#
# #1902 -- the gate must key on the EXPLICIT `verifybin` results ONLY, never on a
# blanket grep of the whole log.  `loadbin` runs its own internal post-program
# verify and prints "Verification failed @ address ..." / "Error while programming
# flash: Verify failed." from inside the reset race described above.  A blanket
# grep matched THOSE lines and failed the run even when both explicit `verifybin`
# passes reported "Verify successful." on the full image -- i.e. it reported a
# GOOD flash as a failure and exited before ever booting the app or reading its
# console.  Bench-measured 2026-09-04 on aen-qenc-readout: internal verify failed
# at 0x80010000, `verifybin` of all 77776 bytes @0x80010000 and all 5552 bytes
# @0x8057EA50 both succeeded.
#
# This does NOT weaken corruption detection.  `verifybin` reads the image back off
# the part and compares every byte; if the bytes are wrong it prints "Verify
# failed." and the 2/2 count below does not reach 2, so the script still exits 3.
# The count also catches the quieter case -- a run that aborted before the
# verifies executed reports neither success nor failure, and a "no news is good
# news" gate would pass it.
#
# The count is necessary, not sufficient: `verifybin` reads through the same debug
# AP, so the app's own `RESULT` line off the RAM console in step 4 remains the
# only end-to-end proof that the flashed image actually runs.
# #1902 -- DIAGNOSE A FAILED HALT EXPLICITLY.  With `, noreset` on the loadbins,
# the single explicit `RSetType 2; r; h` above is the ONLY reset in the sequence,
# so if it does not actually halt the core there is no longer any fallback: both
# loadbins then run against a LIVE CPU, cannot preserve the RAMCode workspace at
# 0x00000000-0x0001FFFF (WorkRAMAddr/WorkRAMSize from device.xml), and write
# NOTHING.  The verify gate below still catches that correctly -- but it reports
# it as "verify failed", which reads like corrupted MRAM when in fact not one byte
# was written.  Bench-measured 2026-09-04 over 12 runs: verify failed if and only
# if the halt failed, 12/12.
#
# What decides it is whether the AHB-AP at
#   AP[3] (APAddr 0x00300000): AHB-AP (IDR: 0x34770008)
# reappears in J-Link's post-reset CoreSight scan.  When it does, `AP[3]: Core
# found` and J-Link manually halts a core the SES has PARKED at VTOR 0 -- every
# successful dump reads `PC = 0000000C`, `CycleCnt = 00000000`,
# `IPSR = 000 (NoException)` -- and the SES then leaves it alone for the whole
# ~5 s program.  When AP[3] is absent the scan stops at
#   AP[2] (APAddr 0x00070000): AXI-AP
# and prints `Could not find core in Coresight setup`; the HE debug domain is
# unreachable, so nothing inside `r` can halt anything.
#
# NOTE the halt NEVER catches the reset vector, not even when it works -- the
# working mechanism is a manual halt of the SES-parked core, never a
# DEMCR.VC_CORERESET catch.  `Reset: VC_CORERESET did not halt CPU. (Debug logic
# also reset by reset pin?).` is SEGGER's generic guess and is printed IDENTICALLY
# in passing runs; do NOT read it as evidence the pin reset cleared debug logic.
#
# A `Verify failed` in such a run says NOTHING about MRAM content: `Expected D0
# read AA` alongside `Could not read memory.` is the gated-DAP floating-read tell,
# not a byte comparison.  Name this failure for what it is.
if grep -qiE "Failed to halt CPU|Cannot read register 16 \(XPSR\) while CPU is running|Failed to preserve target RAM" /tmp/flowd-mramxip.out; then
  echo "!! HALT FAILED -- the core was RUNNING for the whole flash sequence."
  grep -iE "Core did not halt after reset|VC_CORERESET did not halt CPU|Failed to halt CPU|Cannot read register 16 \(XPSR\) while CPU is running|Failed to preserve target RAM" /tmp/flowd-mramxip.out | head -5
  echo "   NOTHING was written: with 'loadbin ..., noreset' there is no fallback reset,"
  echo "   so the RAMCode workspace could not be preserved and both blobs were skipped."
  echo "   slot0 still holds whatever was there before -- this is NOT MRAM corruption."
  echo "   Retry; if it persists, erase slot0 over the SE-UART so the SES boots nothing"
  echo "   and the core cannot be running when the next flash starts."
  exit 5
fi
if grep -qiE "verification failed @|error while programming flash" /tmp/flowd-mramxip.out; then
  echo "?? loadbin reported an internal verify error -- NOT failing on it (#1902)."
  grep -iE "verification failed @|error while programming flash" /tmp/flowd-mramxip.out | head -3
  echo "   Deferring to the explicit verifybin results below, which read the whole image back."
fi
verify_ok=$(grep -ci "verify successful" /tmp/flowd-mramxip.out || true)
if grep -qiE "^Verify failed\.|mismatch" /tmp/flowd-mramxip.out; then
  echo "!! VERIFY FAILED -- the bytes on the part do NOT match the image."
  grep -iE "^Verify failed\.|mismatch" /tmp/flowd-mramxip.out | head -5
  echo "   slot0 content is NOT what you built.  Do not treat this board as flashed."
  exit 3
fi
if [ "${verify_ok:-0}" -lt 2 ]; then
  echo "!! only ${verify_ok:-0} of 2 verifybin passes reported success -- treating as FAILED."
  echo "   (expected one per loadbin: the app image and the AppTocPackage.)"
  exit 3
fi
echo "verify: ${verify_ok}/2 verifybin passes OK (app image + AppTocPackage)"

# 4. SES has re-booted the app; attach read-only (generic device) + dump RAM console.
sleep 3
if [ -z "$BUF_SYM" ]; then
  echo "----- $NAME RAM console: no 'ram_console_buf' in this image (UART-console app) -----" >&2
  echo "      the flash above still completed -- this is not a boot failure. Read the" >&2
  echo "      console via the labgrid 'console' resource instead." >&2
else
  cat > /tmp/flowd-mramxip-read.jlink <<EOF
$SEL
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem8 $BUF, $SIZE
exit
EOF
  $JLINK -nogui 1 -CommanderScript /tmp/flowd-mramxip-read.jlink 2>/tmp/flowd-mramxip-rd.err > /tmp/flowd-mramxip-rd.out || true
  # JLinkExe exits 0 even when it never opened the probe, so `|| true` above
  # hides a total connect failure and the decode below would render it as
  # empty target output (alp-sdk#1318).
  bench_jlink_assert_connected /tmp/flowd-mramxip-rd.out "Flow D mramxip read-back" || exit 7
  echo "----- $NAME RAM console (flow-D MRAM-XIP flashed, SE-booted) -----"
  awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>6)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/flowd-mramxip-rd.out
  echo; echo "--------------------------------------------------------"
fi
