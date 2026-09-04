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
# This script sidesteps that profile entirely: it registers a custom device that
# pairs the WORKING generic Cortex-M55 connect with Alif's OWN flash algorithm,
# taken from the CMSIS pack (`Flash/algorithms/Ensemble.FLM`).
#
# THE SECTOR-SIZE PATCH (the part that actually took the digging)
# --------------------------------------------------------------
# Using Alif's .FLM unmodified still fails, with a J-Link DLL log ending:
#
#     Flash bank @ 0x80000000: SFL: Parsing sectorization info from ELF file
#       FlashDevice.SectorInfo[0]: .SectorSize = 0x00580000, .SectorStartAddr = 0
#      -- Start of determining dirty areas in flash cache
#       ***** Internal Error:
#
# The algorithm's own descriptor reads:
#
#     DevName 'Ensemble 5.5MB MRAM'  DevAdr 0x80000000  szDev 0x00580000
#     szPage  0x400                  valEmpty 0x00
#     sectors: szSector=0x00580000 AddrSector=0x00000000      <-- ONE 5.5 MB sector
#
# It programs in 1 KB pages but declares the whole 5.5 MB as a single SECTOR.
# J-Link treats a sector as its read-modify-write unit, so any partial write
# tries to buffer 5.5 MB and throws an internal error.  MRAM is byte-writable and
# does not need erase-before-write, so the giant sector is a NOR-flash fiction.
#
# Patching a COPY of the .FLM to declare 1 KB sectors -- matching the real page
# size -- makes the cache work in page-sized units and programming succeeds:
#
#     J-Link: Flash download: Bank 0 @ 0x80000000: 1 range affected (1024 bytes)
#     J-Link: Flash download: Program & Verify speed: 29 KB/s
#
# VERIFIED on silicon 2026-09-04 (E1M-AEN801 2026W36-0001, E8 Rev A1):
#   - a 16-byte probe write read back correctly AND survived a cold power-cycle
#     (the real bar -- `verifybin` alone has historically passed on writes the
#     SES never committed)
#   - the full flash-jlink-mramxip.sh flow then ran end to end: 2/2 verifybin,
#     slot0 = 20011B88 8001571D matching the built image, and the app BOOTED
#     MRAM-XIP -- `ROM 5632 KB` in the banner, not the 256 KB of an ITCM load
#   - slot0 still matched after another cold power-cycle
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

struct.pack_into("<II", d, SECTORS, page, 0x00000000)
open(dst, "wb").write(bytes(d))
print(f"   patched   : szSector=0x{page:08x} (matches the programming page size)")
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
