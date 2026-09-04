#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Install a custom J-Link device that unlocks Flow D (MRAM programming) on probes
# that CANNOT select SEGGER's built-in Alif part profile.
#
# WHY THIS EXISTS
# ---------------
# Flow D writes the app to MRAM slot0, and MRAM -- unlike ITCM -- needs a flash
# algorithm.  scripts/bench/aen/flash-jlink-mramxip.sh gets that algorithm by
# selecting SEGGER's built-in `AE822FA0E5597LS0_M55_HE` part profile, which on
# some probes fails at connect:
#
#     Device "AE822FA0E5597LS0_M55_HE" selected.
#     Error occurred: Could not connect to the target device.
#
# Bench-observed 2026-09-04 on J-Link `000600107451` (Hardware version V10.10):
# the GENERIC `Cortex-M55` device connects fine on the same probe and board --
# `Found SW-DP with ID 0x4C013477`, memory reads work, Flow C RAM-runs work --
# and the part profile fails under ALL FIVE reset strategies (RSetType 0/1/2/3/8).
# So the probe is fine for debug, halt, register access and RAM loading; only the
# built-in Alif profile refuses.
#
#
# WHY THE BUILT-IN PROFILE REFUSES -- MEASURED 2026-09-04, not assumed.
# It is a J-Link PROBE CAPABILITY gate, not anything about the board, the AP map
# or the silicon revision.  With `-log` enabled the real cause appears (the
# console only ever prints "Could not connect to the target device."):
#
#     ConfigTargetSettings() end - Took 9us
#     JLINK_EMU_HasCapEx(0x00000052)
#     - 0.001ms returns 0
#     - 0.308ms returns 0xFFFFFEFA
#
# JLINK_Connect() fails in 0.308 ms on that capability check, BEFORE any SWD line
# activity -- which is why `Found SW-DP with ID 0x4C013477` never prints on this
# path.  Probe S/N 600107451 (Hardware version V10.10, Firmware "J-Link V10
# compiled Jan 30 2023") returns 0 for capability 0x52.
#
# Isolated by control: applying `exec SetDormantModeHandling = 1` to the GENERIC
# Cortex-M55 device -- which connects perfectly without it -- reproduces the Alif
# profile's failure byte-for-byte, same HasCapEx(0x00000052) -> 0, same
# 0xFFFFFEFA, same sub-millisecond abort.
#
# The reason the profile needs it: the Ensemble is an SWD MULTI-DROP part
# (AE822FA0E5597LS0_M55_HE / _M55_HP / _A32_0 / _A32_1 all sit on one DP).  The
# profile's embedded J-Link script, extracted from libjlinkarm, contains
# `SetSWDTargetId=0x01002927` and `SetSWDInstanceId=0x1` / `=0x0`, so connecting
# requires dormant-mode / multi-drop target selection.
#
# THERE IS NO BYPASS.  `SetDormantModeHandling = 0` is accepted by the DLL but the
# profile re-asserts its own settings inside ConfigTargetSettings() at connect, so
# a user `exec` is overwritten; and the DLL contains no SkipDormant /
# DisableMultidrop / force-legacy-SWD command.  An AP-map override was also tried
# across six variants and disproven -- the connect never reaches AP enumeration.
#
# So Flow D via the built-in loader needs a probe whose firmware has capability
# 0x52 (the skill's "J-Link V13 fw" row is this, now with the mechanism behind
# it).  It cannot be fixed from the host.
# This script sidesteps that profile entirely: it registers a custom device that
# pairs the WORKING generic Cortex-M55 connect with Alif's OWN flash algorithm,
# taken from the CMSIS pack (`Flash/algorithms/Ensemble.FLM`).
#
# THE SECTOR-SIZE PATCH (necessary, and NOT the Flow D fix -- read before trusting)
# ------------------------------------------------------------------------------
# Using Alif's .FLM unmodified fails outright, with a J-Link DLL log ending:
#
#     Flash bank @ 0x80000000: SFL: Parsing sectorization info from ELF file
#       FlashDevice.SectorInfo[0]: .SectorSize = 0x00580000, .SectorStartAddr = 0
#      -- Start of determining dirty areas in flash cache
#       ***** Internal Error:
#
# The algorithm programs in 1 KB pages but declares the whole 5.5 MB as ONE sector.
# J-Link treats a sector as its read-modify-write unit, so any partial write tries
# to buffer 5.5 MB and throws.  Patching a COPY to a smaller sector gets past that.
#
# !! CORRECTION (2026-09-04).  Commits 441b48220 and 4c504e7ef both credited the
# !! SECTOR SIZE as "the fix" for Flow D's intermittent failures.  That was
# !! concluded from single passing samples and is WRONG.  The sector patch only
# !! makes the loader USABLE at all; it has nothing to do with why writes then
# !! failed intermittently.  The actual cause is the implicit reset in `loadbin`
# !! -- see the #1902 comment block in flash-jlink-mramxip.sh.  On the E8 that
# !! AIRCR.SYSRESETREQ resets the Secure Enclave too, the SES re-boots slot0, and
# !! the M55 executes XIP out of the MRAM being programmed.  Fixed there with
# !! `loadbin ..., noreset`, not here.
#
# !! ALSO WRONG, and previously written into this header: that MRAM's
# !! valEmpty = 0x00 means "a write over non-erased MRAM cannot flip bits back".
# !! MRAM has no erase physics -- it is byte-writable and `valEmpty` is a Keil
# !! FlashDevice convention, not a device property.  What an un-erased write
# !! actually fights is J-Link's own dirty-page bookkeeping, not the array.
#
# SECTOR SIZE = the FLM's 1 KB page size, deliberately.  Making the sector equal
# the page means J-Link never has to MERGE: no sector is part-written, so it never
# needs to pre-READ the surrounding bytes to reconstruct them.  That matters
# because debug-AP reads of this part are documented to lie in some states (see
# reference_aen_e8_bench_traps) -- and under a 64 KB sector a lying pre-read would
# make J-Link write ZEROS over up-to-64 KB of perfectly good neighbouring content
# it thought it was preserving.  1 KB bounds that blast radius to one page.
# An earlier comment here claimed 1 KB "costs reliability"; that too was a single
# sample and is withdrawn.  The REAL cost of 1 KB is THROUGHPUT, and it is
# measured: a ~108 KB image becomes ~106 sector erases and programs at 29 KB/s,
# versus ~2 erases and 82-196 KB/s at 64 KB.  That trade is taken deliberately --
# a slow flash is worth bounding how much good neighbouring content a lying
# debug-AP pre-read can zero.
#
# ALWAYS ERASE FIRST anyway -- it keeps J-Link's dirty-area detection trivially
# correct and removes the merge path entirely.  Verify the erase actually
# happened: the SETOOLS maintenance menu silently does nothing if its prompts
# desync, so read slot0 back rather than trusting "Full Erase done".
#
# !! NOT A DAMAGE CASE (correcting an earlier claim in this file): the address
# !! 0x8057fe50 is NOT an SES-owned structure and was NOT destroyed by a 64 KB
# !! erase.  It is `app-device-config.bin` (0x138 = 312 bytes) INSIDE the app
# !! package we write ourselves at 0x8057ea50, per SETOOLS'
# !! build/app-package-map.txt.  Every successful package write recreates it.
# !! It disappeared from `gettoc` because the package write did not land, not
# !! because anything erased across into SES territory.  No Alif escalation.
#
# The supported alternative is a J-Link V11+ probe, which can select SEGGER's own
# AE822FA0E5597LS0_M55_HE profile and needs none of this.
#
# USAGE
#   bash scripts/bench/aen/install-jlink-alif-device.sh
#   JLINK_DEVICE_FLASH=AE822_ALP_M55_HE JLINK_SN=<probe-sn> \
#       bash scripts/bench/aen/flash-jlink-mramxip.sh <build-dir>
#
# Nothing outside $HOME is touched; the stock J-Link install is left alone.
set -euo pipefail

DFP="${ALIF_DFP_DIR:-/home/caner/alif-dfp-ref}"
SRC_FLM="$DFP/Flash/algorithms/Ensemble.FLM"
DEST="${JLINK_DEVICES_DIR:-$HOME/.config/SEGGER/JLinkDevices}/AlifSemiconductor/AE822_ALP"
DEV_NAME="AE822_ALP_M55_HE"

[ -f "$SRC_FLM" ] || {
	echo "!! missing $SRC_FLM" >&2
	echo "   Set ALIF_DFP_DIR to a checkout of the Alif CMSIS pack." >&2
	exit 1
}

mkdir -p "$DEST"

# Patch sectors[0] from one 5.5 MB sector to 1 KB sectors.  The offset is derived
# from the Keil FlashOS FlashDevice layout, and the script REFUSES to write if the
# bytes it finds are not the expected 0x00580000/0x00000000 -- so a future pack
# with a different descriptor fails loudly instead of being silently corrupted.
python3 - "$SRC_FLM" "$DEST/Ensemble.FLM" <<'PY'
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())

# DevDscr section starts at file offset 0x210.  Within it:
#   u16 Vers | char DevName[128] | u16 DevType | u32 DevAdr | u32 szDev
#   u32 szPage | u32 Res | u8 valEmpty (+3 pad) | u32 toProg | u32 toErase
#   then FlashSectors[] { u32 szSector, u32 AddrSector }
DEV_DSCR = 0x210
SECTORS  = DEV_DSCR + 160

name  = d[DEV_DSCR + 2: DEV_DSCR + 2 + 128].split(b"\0", 1)[0].decode("ascii", "replace")
page  = struct.unpack_from("<I", d, DEV_DSCR + 140)[0]
szs, adr = struct.unpack_from("<II", d, SECTORS)

print(f"   algorithm : {name}")
print(f"   page size : 0x{page:x}")
print(f"   sectors[0]: szSector=0x{szs:08x} AddrSector=0x{adr:08x}")

if (szs, adr) != (0x00580000, 0x00000000):
    sys.exit(f"!! unexpected sector descriptor -- refusing to patch {src}")

# Equal to the FLM's 1 KB programming page size, so "sector" and "page" coincide
# and J-Link never merges -- hence never pre-reads neighbouring bytes through a
# debug AP that can lie.  See the header note.
SECTOR_SIZE = 0x00000400
struct.pack_into("<II", d, SECTORS, SECTOR_SIZE, 0x00000000)
open(dst, "wb").write(bytes(d))
print(f"   patched   : szSector=0x{SECTOR_SIZE:08x} (1 KB = page size -- no read-modify-write merge)")
PY

cat > "$DEST/device.xml" <<EOF
<!-- Generated by scripts/bench/aen/install-jlink-alif-device.sh -- do not edit.
     Pairs the generic Cortex-M55 connect (which works on probes that cannot
     select SEGGER's built-in Alif part profile) with Alif's own MRAM flash
     algorithm, patched to 1 KB sectors.  Addresses transcribed from
     AlifSemiconductor.Ensemble.pdsc. -->
<Database>
  <Device>
    <ChipInfo Vendor="AlifSemiconductor" Name="$DEV_NAME" Core="JLINK_CORE_CORTEX_M55"
              WorkRAMAddr="0x00000000" WorkRAMSize="0x00020000" />
    <FlashBankInfo Name="MRAM" BaseAddr="0x80000000" AlwaysPresent="1">
      <LoaderInfo Name="Ensemble MRAM" MaxSize="0x00580000"
                  Loader="Ensemble.FLM" LoaderType="FLASH_ALGO_TYPE_OPEN" />
    </FlashBankInfo>
  </Device>
</Database>
EOF

echo
echo ">>> installed device '$DEV_NAME' -> $DEST"
echo "    Flow D:  JLINK_DEVICE_FLASH=$DEV_NAME JLINK_SN=<sn> \\"
echo "               bash scripts/bench/aen/flash-jlink-mramxip.sh <build-dir>"
