#!/usr/bin/env bash
# scripts/bench/aen/flash-run-dualcore.sh <hp-build-dir> <he-build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives the Alif SETOOLS over the SE-UART + JLinkExe, both Linux
# binaries on this bench). Runs under WSL2 on Windows (with USB serial
# passed through). See docs/aen-bench-bringup.md.
#
# FLOW A, two-entry ATOC for the dual-core DEFERRED-TOC boot recipe
# (CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC). Generalizes
# flash-run.sh's single M55-HE entry to a master + peer pair:
#   - ALP-HP (master): M55_HP, loadAddress 0x50000000, flags ["load","boot"]
#     -- the SES releases it normally at power-on, same as any single-core
#     image.
#   - ALP-HE (peer):   M55_HE, loadAddress 0x58000000, flags
#     ["load","boot","deferred"] -- the SETOOLS guide (AUGD0005 p.35) defines
#     deferred as "skipped at boot time (i.e., no boot OR LOAD)": the SES
#     places NOTHING in ITCM at power-on (TOC_IMAGE_DEFERRED=0x100; shows
#     "uLs  D" in the SES boot table, Dest Addr blank, Time 0.00 ms) and does
#     the load at runtime instead, inside the service-500 call below.
#
# NOTE ON WHY: for THIS direction (HP master -> HE peer), plain
# ["load","boot"] + se_service_boot_cpu() (service 501) also works --
# bench-proven on E8 both 2026-06-17 and 2026-08-01. This script's deferred
# shape is not a bug fix for HE; it demonstrates the recipe that is the ONLY
# proven way to release an HP peer (HE-master -> HP-peer), where 501 fails
# for a real, vendor-documented reason: Alif's SE Host Services API docs
# (v1.109.0 p.115) name "the M55-HP core in Ensemble devices" as a case
# where resetting the core (which service 501's release does) invalidates
# its TCM content -- measured on E8 (2026-07-31, HE master releasing an HP
# peer via 501) as CFSR=0x00000101 IACCVIOL+IBUSERR, PC=0xEFFFFFFE. p.115's
# own remedy is reset -> reload -> release, i.e. the reset still happens;
# the fix is loading AFTER it. Service 500 (se_service_process_toc_entry(),
# used here) does exactly that in one call -- the plain path instead loads
# at power-on, BEFORE 501's release resets the core -- so 500 works both
# directions -- see src/backends/mproc/alif_se_boot.c and the
# CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC Kconfig help for the full
# asymmetry table and vendor citations.
#
# The HP image's build MUST set:
#   CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC=y
#   CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_ENTRY_ID="ALP-HE"
# (the default entry id already matches this script's ALP-HE key -- only
# override CONFIG_..._ENTRY_ID if you rename the ATOC key below.)
#
# Unlike flash-run.sh this does not read a RAM console back automatically
# -- a dual-core bring-up wants proof from BOTH cores. Read the HP core's
# console with reread.sh; read the HE core's liveness however your app
# proves it (an SRAM0 beacon like flash-jlink-hp.sh's, an RPMsg PING/PONG
# counter over <alp/rpc.h>, etc).
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. Export SE_UART to the SE-UART
# serial device (host-specific). See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

HP_BD="$1"
HE_BD="$2"
if [ -z "$HP_BD" ] || [ -z "$HE_BD" ]; then
	echo "usage: $0 <hp-build-dir> <he-build-dir> [post_boot_read_bytes_hex]" >&2
	exit 2
fi
bench_require_setools || exit $?
if [ -z "${SE_UART:-}" ]; then
	echo "flash-run-dualcore: SE_UART is unset — export it to the SE-UART serial device" >&2
	echo "                    (Linux: /dev/ttyUSB*, macOS: /dev/cu.usbserial-*, Windows/WSL: passed-through COM)." >&2
	exit 2
fi
SET="$SETOOLS_DIR"
HP_NAME=$(basename "$HP_BD")
HE_NAME=$(basename "$HE_BD")
HP_BIN="$HP_BD/zephyr/zephyr.bin"
HE_BIN="$HE_BD/zephyr/zephyr.bin"
[ -f "$HP_BIN" ] || { echo "missing HP zephyr.bin: $HP_BIN" >&2; exit 2; }
[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }

# 1. stage both images + a two-entry signed-ATOC config (keeps the
#    factory DEVICE cfg). ALP-HP is the master; ALP-HE is the deferred
#    peer the HP image un-defers at runtime via service 500.
cp -f "$HP_BIN" "$SET/build/images/$HP_NAME.bin"
cp -f "$HE_BIN" "$SET/build/images/$HE_NAME.bin"
cat > "$SET/build/config/dualcore.json" <<JSON
{
    "DEVICE": { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HP": { "disabled": false, "binary": "$HP_NAME.bin", "version": "1.0.0", "signed": true,
                "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] },
    "ALP-HE": { "disabled": false, "binary": "$HE_NAME.bin", "version": "1.0.0", "signed": true,
                "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot", "deferred"] }
}
JSON

# #1069 ATOC-assembly guard: both entries above use loadAddress (ITCM,
# already disjoint by construction -- 0x50000000 HP vs 0x58000000 HE),
# not mramAddress, so this is a no-op today. It's here so a future
# slot0-XIP dual-core recipe added to this script inherits the same
# window/overlap check alif_flash.py's runner uses, instead of
# reintroducing the collision this issue fixes. See scripts/aen_atoc.py.
python3 "$ALP_SDK_DIR/scripts/aen_atoc.py" "$SET/build/config/dualcore.json" || exit 1

cd "$SET"
echo ">>> FLASH dual-core HP=$HP_NAME (master) HE=$HE_NAME (deferred peer, entry id ALP-HE)" >&2
./app-gen-toc -f "build/config/dualcore.json" >/tmp/gentoc-dual.log 2>&1 || { echo "gen-toc FAILED"; tail /tmp/gentoc-dual.log; exit 1; }
# 2. write to MRAM over the SE-UART (SES auto-enters maintenance, burns,
#    resets+boots ALP-HP; ALP-HE stays loaded-but-not-released until the
#    HP image un-defers it at runtime).
./app-write-mram -c "$SE_UART" -p >/tmp/wrmram-dual.log 2>&1 || true
if grep -q "Done" /tmp/wrmram-dual.log; then echo "MRAM write: Done ($(grep -oE '[0-9]+\.[0-9]+ seconds' /tmp/wrmram-dual.log | tail -1))"; else echo "MRAM write FAILED:"; tail -5 /tmp/wrmram-dual.log; exit 1; fi

echo "----- dual-core ATOC flashed -----" >&2
echo "confirm ALP-HE shows 'uLs  D' (deferred, Dest Addr blank, Time 0.00 ms) in the SES boot table --" >&2
echo "NOT 'uLV' (that would mean the 'deferred' flag didn't take and the SES already released it," >&2
echo "which the plain 501 release path handles fine for an HE peer -- this script exists for the" >&2
echo "HP-peer direction, where 501 is vendor-documented broken; see the script header)." >&2
echo "then confirm HP un-defers + releases HE via your app's IPC proof (RPMsg link, SRAM0 beacon, etc)." >&2
