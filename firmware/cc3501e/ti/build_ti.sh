#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Linux port of build_ti.ps1 -- build the PRODUCTION cc3501e-bridge image
# (CC3501E_HAL_BACKEND=ti) with TI ticlang + the SimpleLink Wi-Fi SDK (CC35xx).
#
# Mirrors the validated SysConfig -> compile -> link recipe (SDK 10.10.01.08 +
# ticlang 5.1.1). See build_ti.ps1 for the full rationale on every choice (the
# unity FreeRTOS aggregates, the connectivity vendor linker.cmd / 512K DRAM, the
# --reread_libs circular group, the FWU/OTFDE cold-boot fix). This file keeps the
# SAME include sets, defines, sources, and link order; only paths + tool names
# are Linux-ified.
#
# Usage:
#   ./build_ti.sh                                  # default SPI bridge, no WiFi/BLE
#   SDK_DIR=<ti-sdk>/simplelink_wifi_sdk_10_10_01_08 ./build_ti.sh --wifi
#   ./build_ti.sh --wifi --ble                     # + WiFi host driver + NimBLE
#   ./build_ti.sh --transport sdio --ota-selftest
#
# The TI SDK / toolchain / SysConfig / toolbox locations are NOT bundled;
# point at your staged copies via the env vars (or flags) below.
#
# Output: <repo>/firmware/cc3501e/build/ti/cc3501e-bridge.{out,hex,bin}
set -euo pipefail

# --- config (override via env or flags) --------------------------------------
# No defaults: these point at externally-staged TI tooling and are validated
# below so the script fails with a clear message on any workstation/CI checkout.
SDK_DIR="${SDK_DIR:-}"
TICLANG_ROOT="${TICLANG_ROOT:-}"
SYSCONFIG_CLI="${SYSCONFIG_CLI:-}"
# SimpleLink Wi-Fi Toolbox -- provides the SysConfig MemoryConfigurator module
# (/ti/memoryconfig/MemoryConfigurator) that the main SDK product does NOT ship.
# Passed as a SECOND --product to generate the flash-map (see the demo makefile's
# SIMPLELINK_WIFI_TOOLBOX_INSTALL_DIR). TOOLBOX = the dir holding .metadata/product.json.
TOOLBOX="${TOOLBOX:-}"
TRANSPORT="spi"          # spi | sdio
OTA_SELFTEST=0
WIFI_HOST_DRIVER=0
BLE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --transport) TRANSPORT="$2"; shift 2 ;;
    --ota-selftest) OTA_SELFTEST=1; shift ;;
    --wifi) WIFI_HOST_DRIVER=1; shift ;;
    --ble) BLE=1; shift ;;
    --sdk) SDK_DIR="$2"; shift 2 ;;
    --ticlang) TICLANG_ROOT="$2"; shift 2 ;;
    --sysconfig) SYSCONFIG_CLI="$2"; shift 2 ;;
    --toolbox) TOOLBOX="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
# -Ble implies -WifiHostDriver (shared HIF -> Wlan_Start first; NimBLE reuses the
# Wi-Fi OSI/Report seam). See build_ti.ps1 lines 33-37.
[ "$BLE" = 1 ] && WIFI_HOST_DRIVER=1

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$(cd "$HERE/.." && pwd)"                 # firmware/cc3501e
repo="$(cd "$fw/../.." && pwd)"              # repo root
out="$fw/build/ti"
tc="$TICLANG_ROOT/bin/tiarmclang"
# Require the externally-staged TI tooling before touching it, so an
# unset/empty var fails with a clear message instead of a confusing MISSING
# path (e.g. "/.metadata/product.json").
for v in SDK_DIR TICLANG_ROOT SYSCONFIG_CLI TOOLBOX; do
  [ -n "${!v}" ] || { echo "set $v (env var or the matching --flag) to your staged TI SDK/toolchain/SysConfig/toolbox -- see build_ti.ps1 for the reference layout"; exit 3; }
done
for p in "$SDK_DIR/.metadata/product.json" "$tc" "$SYSCONFIG_CLI" "$TOOLBOX/.metadata/product.json"; do
  [ -e "$p" ] || { echo "MISSING: $p (stage the TI SDK/toolchain/toolbox -- see docs)"; exit 3; }
done
mkdir -p "$out"

echo "== SysConfig: generate the board file (CONFIG_SPI_0) =="
# -wifi uses the DERIVED board file that adds CONFIG_UART2_0 (the Wi-Fi console
# glue opens UART2). See build_ti.ps1 lines 47-53.
if [ "$WIFI_HOST_DRIVER" = 1 ]; then syscfgFile="$HERE/cc3501e_aen_wifi.syscfg"; else syscfgFile="$HERE/cc3501e_aen.syscfg"; fi
"$SYSCONFIG_CLI" --product "$SDK_DIR/.metadata/product.json" --compiler ticlang \
    --output "$out" "$syscfgFile"

echo "== SysConfig: MemoryConfigurator -> flash map (memcfg) =="
# The MemoryConfigurator module ships in the Wi-Fi TOOLBOX, not the SDK -- pass BOTH
# products (mirrors the demo makefile). Produces $out/memcfg/ti_flash_map_config.c
# (the vendor_image_*/bl2_* slot symbols FWU.a references) + its own linker toolbox
# file. NB: the connectivity linker.cmd instead #includes the STUB we emit at $out/
# (see below); the memcfg toolbox output is not used for the link.
"$SYSCONFIG_CLI" --compiler ticlang \
    --product "$SDK_DIR/.metadata/product.json" --product "$TOOLBOX/.metadata/product.json" \
    --output "$out/memcfg" "$HERE/cc3501e_mem.syscfg"

inc=(
  "-I$fw/src" "-I$fw/hal" "-I$repo/include" "-I$out" "-I$out/memcfg"
  "-I$SDK_DIR/source" "-I$SDK_DIR/source/ti/utils/FWU/headers" "-I$SDK_DIR/kernel/freertos" "-I$SDK_DIR/source/ti/posix/ticlang"
  "-I$SDK_DIR/source/third_party/freertos/include"
  "-I$SDK_DIR/source/third_party/freertos/portable/GCC/ARM_CM33_NTZ/non_secure"
)
cflags=(-c -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
        -DDeviceFamily_CC35XX -DCC35XX -DCC3501E_RTOS_FREERTOS -Oz
        -ffunction-sections -fdata-sections -Wall)

# Derive the GET_DIAG_INFO.fw_version marker from firmware-version.txt (the SINGLE
# source of truth) so this TI build path stays in lockstep with the CMake build and
# never drifts from the release version.  Pre-1.0 packing: (MINOR<<8)|PATCH -> 0.2.0
# = 0x0200.  This is the APP SemVer marker -- DISTINCT from the GPE flash version in
# deploy_validate.sh (anti-rollback gate) and ALP_CC3501E_PROTOCOL_VERSION (wire gate).
fwver="$(head -n1 "$fw/firmware-version.txt" | tr -d '[:space:]')"
if [[ "$fwver" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  fw_u16=$(printf '0x%04x' $(( (${BASH_REMATCH[2]} << 8) | ${BASH_REMATCH[3]} )))
else
  echo "firmware-version.txt not SemVer major.minor.patch: '$fwver'" >&2; exit 4
fi
echo "== fw_version marker: $fwver -> $fw_u16 (from firmware-version.txt) =="
cflags+=("-DCC3501E_BRIDGE_FW_VERSION_U16=$fw_u16")

txdef=()
[ "$TRANSPORT" = sdio ] && txdef+=(-DCC3501E_CONTROL_TRANSPORT_SDIO=1)
[ "$OTA_SELFTEST" = 1 ] && txdef+=(-DCC3501E_OTA_SELFTEST)

ntDir="$SDK_DIR/examples/rtos/LP_EM_CC35X1/demos/network_terminal"
if [ "$WIFI_HOST_DRIVER" = 1 ]; then
  txdef+=(-DCC3501E_WIFI=1)
  inc+=("-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_host_driver/inc_adapt"
        "-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_host_driver/inc_common"
        "-I$SDK_DIR/source/ti/net/wifi_stack/inc_common"
        "-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_platform/cc35xx/inc_common"
        "-I$ntDir" "-I$ntDir/adaptation"
        "-I$SDK_DIR/source/third_party/mbedtls/include"
        "-I$SDK_DIR/source/third_party/mbedtls/ti/configs"
        "-I$SDK_DIR/source/third_party/mbedtls/ti/port"
        "-I$SDK_DIR/source/third_party/lwip/lwip-stack/src/include"
        "-I$SDK_DIR/source/third_party/lwip/ti_config/lwip-port/osi/include"
        "-I$SDK_DIR/source/third_party/lwip/lwip-contrib")
  txdef+=(-DSNTP_SUPPORT -DNVOCMP_SPS_USE_CBC -DNVOCMP_POSIX_MUTEX
          '-DMBEDTLS_CONFIG_FILE="config-hsm.h"'
          '-DMBEDTLS_PSA_CRYPTO_CONFIG_FILE="config-psa-crypto-hsm.h"')
fi
if [ "$BLE" = 1 ]; then
  txdef+=(-DCC3501E_BLE=1
          -DMYNEWT_VAL_BLE_MAX_CONNECTIONS=2 -DMYNEWT_VAL_BLE_MULTI_ADV_INSTANCES=1
          -DMYNEWT_VAL_MSYS_1_BLOCK_COUNT=24 -DMYNEWT_VAL_BLE_STORE_MAX_BONDS=4
          -DMYNEWT_VAL_BLE_STORE_MAX_CCCDS=4 -DMYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM=1)
  nimbleRoot="$SDK_DIR/source/third_party/nimble"
  inc+=("-I$SDK_DIR/source/ti/net/ble_interface/inc_adapt"
        "-I$SDK_DIR/source/ti/net/ble_interface/inc_common"
        "-I$nimbleRoot/ti_config/nimble-port/include"
        "-I$nimbleRoot/ti_config/nimble-port/include/console"
        "-I$nimbleRoot/ti_config/nimble-port/include/hal"
        "-I$nimbleRoot/ti_config/nimble-port/include/syscfg"
        "-I$nimbleRoot/ti_config/nimble-port/porting/nimble/include"
        "-I$nimbleRoot/ti_config/nimble-port/porting/npl/osi/include"
        "-I$nimbleRoot/ti_config/nimble-port/transport/cc3xxxhif/include"
        "-I$nimbleRoot/nimble-src/nimble/include"
        "-I$nimbleRoot/nimble-src/nimble/host/include"
        "-I$nimbleRoot/nimble-src/nimble/host/services/dis/include"
        "-I$nimbleRoot/nimble-src/nimble/host/services/gap/include"
        "-I$nimbleRoot/nimble-src/nimble/host/services/gatt/include"
        "-I$nimbleRoot/nimble-src/nimble/host/store/config/include"
        "-I$nimbleRoot/nimble-src/nimble/host/store/ram/include"
        "-I$nimbleRoot/nimble-src/nimble/host/util/include"
        "-I$nimbleRoot/nimble-src/nimble/transport/common/hci_h4/include"
        "-I$nimbleRoot/nimble-src/nimble/transport/common/hci_ipc/include"
        "-I$nimbleRoot/nimble-src/nimble/transport/include"
        "-I$nimbleRoot/nimble-src/porting/nimble/include")
fi

# App + silicon-free layer + ti HAL + SysConfig unity aggregates (see .ps1 149-165).
sources=(
  "$fw/src/main.c" "$fw/src/protocol.c" "$fw"/src/protocol_*.c "$fw/src/worker.c" "$fw/src/event_ring.c" "$fw/src/transport_spi.c" "$fw/src/transport_sdio.c"
  "$fw/hal/ti/cc3501e_hw_ti.c" "$fw/hal/ti/transport_hw_ti_spi.c" "$fw/hal/ti/transport_hw_ti_sdio.c"
  "$out/ti_drivers_config.c" "$out/ti_freertos_config.c" "$out/ti_freertos_portable_config.c"
  "$out/memcfg/ti_flash_map_config.c"
)
# OTFDE flash-decryption driver (FWU.a references otfdeDriver_Config) -- linked
# unconditionally now OTA-over-bridge ships. See .ps1 167-177.
sources+=("$SDK_DIR/source/ti/drivers/net/wifi/wifi_platform/cc35xx/plat/otfde_driver.c")
inc+=("-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_host_driver/inc_adapt"
      "-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_host_driver/inc_common"
      "-I$SDK_DIR/source/ti/drivers/net/wifi/wifi_platform/cc35xx/inc_common"
      "-I$SDK_DIR/source/ti/drivers/xmem/flash")
[ "$OTA_SELFTEST" = 1 ] && sources+=("$fw/hal/ti/cc3501e_ota_candidate.c")

# Wi-Fi host integration sources (source-only OSI/glue) -- see .ps1 186-210.
if [ "$WIFI_HOST_DRIVER" = 1 ]; then
  sources+=("$ntDir/adaptation/osi_dpl.c" "$ntDir/adaptation/osi_filesystem.c"
            "$ntDir/nvocmp_cc35xx.c" "$ntDir/crc.c"
            "$ntDir/adaptation/uart_term.c" "$ntDir/adaptation/syslog.c"
            "$ntDir/network_lwip.c" "$ntDir/dhcpserver.c")
fi
# BLE host integration (NimBLE port source-only) -- see .ps1 212-229.
if [ "$BLE" = 1 ]; then
  nimblePort="$SDK_DIR/source/third_party/nimble/ti_config/nimble-port"
  sources+=("$nimblePort/transport/cc3xxxhif/src/cc3xxxhif_ble_hci.c"
            "$nimblePort/porting/npl/osi/src/npl_os_osi.c"
            "$nimblePort/porting/npl/osi/src/nimble_osi_filesystem.c"
            "$nimblePort/porting/nimble/src/base64.c"
            "$fw/hal/ti/cc3501e_nimble_host.c")
fi

echo "== Compile (${#sources[@]} sources) =="
objs=()
for s in "${sources[@]}"; do
  o="$out/$(basename "$s").o"
  "$tc" "${cflags[@]}" "${txdef[@]}" "${inc[@]}" "$s" -o "$o"
  objs+=("$o")
done

echo "== Linker: VENDOR-APP map (connectivity linker.cmd, FLASH@0x14000000, DRAM 512K) =="
# Emit the SysConfig MemoryConfigurator stub the connectivity cmd #includes, then
# use the network_terminal demo's linker.cmd verbatim. See .ps1 240-273.
cat > "$out/ti_build_linker.cmd.toolbox" <<'EOF'
/* Stub for the SysConfig MemoryConfigurator output the connectivity linker.cmd
 * #includes.  CC3501E external flash = 8 MB (PY25Q64LB); no PSRAM populated. */
#define build_linker_toolbox_FLASH_SIZE 0x00800000
#define build_linker_toolbox_PSRAM_SIZE 0
EOF
stockCmd="$SDK_DIR/examples/rtos/LP_EM_CC35X1/demos/network_terminal/freertos/ticlang/linker.cmd"
localCmd="$out/cc3501e_vendor.cmd"
cp -f "$stockCmd" "$localCmd"

echo "== Link =="
linkcommon=(-Wl,-u,_c_int00 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16)
if [ "$WIFI_HOST_DRIVER" = 1 ]; then
  # FULL Wi-Fi host link set inside a --reread_libs circular group. See .ps1 276-318.
  wifilibs=(
    "$SDK_DIR/source/third_party/lwip/lib/ticlang/lwip.a"
    "$SDK_DIR/source/third_party/hostap/lib/ticlang/hostap.a"
    "$SDK_DIR/source/ti/net/wifi_stack/lib/ticlang/wifi_stack.a"
    "$SDK_DIR/source/ti/drivers/net/wifi/wifi_host_driver/lib/ticlang/wifi_host_driver.a"
    "$SDK_DIR/source/ti/drivers/net/wifi/wifi_platform/cc35xx/lib/ticlang/wifi_platform_cc35xx.a"
    "$SDK_DIR/source/ti/drivers/secure/lib/ticlang/m33f/secure_drivers_cc35xx_mbedtls.a"
    "$SDK_DIR/source/third_party/psa_crypto/lib/ticlang/m33f/psa_crypto_cc35xx.a"
    "$SDK_DIR/source/third_party/mbedtls/ti/lib/ticlang/m33f/mbedtls.a"
    "$SDK_DIR/source/third_party/hsmddk/lib/ticlang/m33f/hsmddk_cc35xx.a"
    "$SDK_DIR/source/third_party/hsmddk/lib/ticlang/m33f/hsmddk_cc35xx_its.a"
    "$SDK_DIR/source/ti/net/ble_interface/lib/ticlang/ble_interface.a"
    "$SDK_DIR/source/ti/utils/FWU/lib/ticlang/FWU.a"
  )
  [ "$BLE" = 1 ] && wifilibs+=("$SDK_DIR/source/third_party/nimble/lib/ticlang/nimble.a")
  wifilibs+=(-Wl,--reread_libs)
  "$tc" "${linkcommon[@]}" "${objs[@]}" "-L$SDK_DIR/source" "${wifilibs[@]}" \
        "$out/ti_utils_build_linker.cmd.genlibs" "$localCmd" \
        -Wl,--rom_model "-Wl,-m,$out/cc3501e-bridge.map" -o "$out/cc3501e-bridge.out"
else
  "$tc" "${linkcommon[@]}" "${objs[@]}" "-L$SDK_DIR/source" \
        "$SDK_DIR/source/ti/utils/FWU/lib/ticlang/FWU.a" \
        "$out/ti_utils_build_linker.cmd.genlibs" "$localCmd" \
        -Wl,--rom_model "-Wl,-m,$out/cc3501e-bridge.map" -o "$out/cc3501e-bridge.out"
fi

"$TICLANG_ROOT/bin/tiarmobjcopy" -O ihex   "$out/cc3501e-bridge.out" "$out/cc3501e-bridge.hex"
"$TICLANG_ROOT/bin/tiarmobjcopy" -O binary "$out/cc3501e-bridge.out" "$out/cc3501e-bridge.bin"
"$TICLANG_ROOT/bin/tiarmsize" "$out/cc3501e-bridge.out"
echo "== Done: $out/cc3501e-bridge.{out,hex,bin} =="
