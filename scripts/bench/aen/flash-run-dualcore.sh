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
#     ["load","boot","deferred"] -- the SES LOADS + VERIFIES the image into
#     ITCM but SKIPS the boot-time release (TOC_IMAGE_DEFERRED=0x100; shows
#     "uLs  D" in the SES boot table, Dest Addr blank, Time 0.00 ms).
#
# This is the FIX for a real silicon bug: an entry flagged ["load"] alone
# (no "deferred") reports "uLV" (Loaded, Verified) in the SES table while
# the destination ITCM holds NO image -- releasing that core with
# se_service_boot_cpu() makes it vector from empty memory and lock up
# (CFSR=0x00000101 IACCVIOL+IBUSERR, PC=0xEFFFFFFE). "deferred" makes the
# SES skip the boot-time action instead of silently mis-reporting it as
# done; the HP image then un-defers + releases HE at runtime with ONE
# se_service_process_toc_entry() call (service 500) -- see
# src/backends/mproc/alif_se_boot.c and the
# CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC Kconfig help.
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
echo "NOT 'uLV' (that would mean the SES thinks it already released an image it never placed)." >&2
echo "then confirm HP un-defers + releases HE via your app's IPC proof (RPMsg link, SRAM0 beacon, etc)." >&2
