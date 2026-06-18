# SPDX-License-Identifier: Apache-2.0
#
# Build the PRODUCTION cc3501e-bridge image (CC3501E_HAL_BACKEND=ti) with
# TI ticlang + the SimpleLink Wi-Fi SDK (CC35xx) on a bench machine.
#
# This script captures the EXACT, validated recipe (SysConfig -> compile ->
# link) that produces a flashable Cortex-M33 image -- proven on 2026-06-16
# against SDK 10.10.01.08 + ticlang 5.1.1.  The CC35xx SDK is FreeRTOS-
# centric (no NoRTOS path via SysConfig), and ti_drivers_config.c +
# ti_freertos_config.c are SysConfig "unity" aggregates that bundle the
# FreeRTOS kernel + the dpl -- so the link uses ONLY those aggregates +
# the prebuilt drivers/driverlib libs (per the generated .genlibs), the
# device startup, and the device linker script.  Do NOT add the raw
# third_party/freertos or kernel/freertos/dpl sources (they collide with
# the aggregates).
#
# Usage (defaults match the 2026-06-16 bench install):
#   ./build_ti.ps1 -SdkDir "C:\Users\<you>\Desktop\ti_simplelink_sdk\simplelink_wifi_sdk_10_10_01_08"
#
# Output: <repo>/firmware/cc3501e/build/ti/cc3501e-bridge.{out,hex,bin}
# Flash the .hex/.bin to the CC3501E over SWD/J-Link (see docs/cc3501e-bridge.md).

param(
    [string]$SdkDir       = "$env:USERPROFILE\Desktop\ti_simplelink_sdk\simplelink_wifi_sdk_10_10_01_08",
    [string]$TiclangRoot  = "C:\ti\ti-cgt-armllvm-5.1.1.LTS\ti-cgt-armllvm_5.1.1.LTS",
    [string]$SysconfigCli = "C:\ti\sysconfig-1.28.0\sysconfig_cli.bat",
    [string]$Transport    = "spi",  # spi | sdio
    [switch]$OtaSelftest,           # build the OTA-self-install validation updater (embeds cc3501e_ota_candidate.c, -DCC3501E_OTA_SELFTEST)
    [switch]$WifiHostDriver         # link the CC35xx Wi-Fi host driver (-DCC3501E_WIFI; enables GET_MAC / scan / connect bodies)
)

$ErrorActionPreference = 'Stop'
$fw   = Split-Path $PSScriptRoot -Parent          # firmware/cc3501e
$repo = (Resolve-Path "$fw\..\..").Path           # repo root
$out  = "$fw\build\ti"
$tc   = "$TiclangRoot\bin\tiarmclang.exe"
New-Item -ItemType Directory -Force $out | Out-Null

Write-Host "== SysConfig: generate the board file (CONFIG_SPI_0) =="
# -WifiHostDriver uses a DERIVED board file that adds CONFIG_UART2_0: the CC35xx
# Wi-Fi host integration's console glue (adaptation/uart_term.c -> Report(),
# syslog.c) opens UART2 and references UART2_config[]/UART2_count, which only
# exist when a UART2 instance is declared.  cc3501e_aen_wifi.syscfg = the bench
# board file + that UART2 instance (on free pins), keeping the silicon-validated
# DEFAULT board file (cc3501e_aen.syscfg) byte-identical for the default build.
$syscfgFile = if ($WifiHostDriver) { "$PSScriptRoot\cc3501e_aen_wifi.syscfg" } else { "$PSScriptRoot\cc3501e_aen.syscfg" }
& $SysconfigCli --product "$SdkDir\.metadata\product.json" --compiler ticlang `
    --output $out $syscfgFile
if ($LASTEXITCODE -ne 0) { throw "SysConfig failed" }

$inc = @(
    "-I$fw\src", "-I$fw\hal", "-I$repo\include", "-I$out", "-I$out\memcfg",
    "-I$SdkDir\source", "-I$SdkDir\source\ti\utils\FWU\headers", "-I$SdkDir\kernel\freertos", "-I$SdkDir\source\ti\posix\ticlang",
    "-I$SdkDir\source\third_party\freertos\include",
    "-I$SdkDir\source\third_party\freertos\portable\GCC\ARM_CM33_NTZ\non_secure")
$cflags = @('-c', '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16',
            '-DDeviceFamily_CC35XX', '-DCC35XX', '-DCC3501E_RTOS_FREERTOS', '-Oz',
            '-ffunction-sections', '-fdata-sections', '-Wall')

$txdef = if ($Transport -eq 'sdio') { @('-DCC3501E_CONTROL_TRANSPORT_SDIO=1') } else { @() }
if ($OtaSelftest) { $txdef = @($txdef) + @('-DCC3501E_OTA_SELFTEST') }
if ($WifiHostDriver) {
    # CC35xx Wi-Fi host driver: enables the real GET_MAC/scan/connect bodies (P0-5/P0-6).
    # The real OSI layer (osi_dpl.c, compiled below) provides osi_uSleep + the ~30-func OSI
    # port, so the FWU busy-wait shim in cc3501e_hw_ti.c is compiled out under CC3501E_WIFI
    # to avoid a multiple-definition.
    $txdef = @($txdef) + @('-DCC3501E_WIFI=1')
    # Include set MIRRORS the SDK network_terminal demo makefile
    # (examples/rtos/LP_EM_CC35X1/demos/network_terminal/freertos/ticlang/makefile) --
    # the authoritative recipe for compiling osi_dpl.c / network_lwip.c / adaptation/*.
    # -I$out = where SysConfig wrote ti_drivers_config.h etc (the makefile's `-I.`);
    # the demo source dir carries default_netif.h / wlan_cmd.h / dhcpserver.h / network_terminal.h.
    $ntDir = "$SdkDir\examples\rtos\LP_EM_CC35X1\demos\network_terminal"
    $inc += @("-I$SdkDir\source\ti\drivers\net\wifi\wifi_host_driver\inc_adapt",
              "-I$SdkDir\source\ti\drivers\net\wifi\wifi_host_driver\inc_common",
              "-I$SdkDir\source\ti\net\wifi_stack\inc_common",
              "-I$SdkDir\source\ti\drivers\net\wifi\wifi_platform\cc35xx\inc_common",
              "-I$ntDir",
              "-I$ntDir\adaptation",
              "-I$SdkDir\source\third_party\mbedtls\include",
              "-I$SdkDir\source\third_party\mbedtls\ti\configs",
              "-I$SdkDir\source\third_party\mbedtls\ti\port",
              "-I$SdkDir\source\third_party\lwip\lwip-stack\src\include",
              "-I$SdkDir\source\third_party\lwip\ti_config\lwip-port\osi\include",
              "-I$SdkDir\source\third_party\lwip\lwip-contrib")
    # network_lwip.c / nvocmp.h pull these (same as the demo CFLAGS).
    $txdef = @($txdef) + @('-DSNTP_SUPPORT', '-DNVOCMP_SPS_USE_CBC', '-DNVOCMP_POSIX_MUTEX',
                           '-DMBEDTLS_CONFIG_FILE="config-hsm.h"',
                           '-DMBEDTLS_PSA_CRYPTO_CONFIG_FILE="config-psa-crypto-hsm.h"')
}

# App + the silicon-free layer + the ti HAL.
$sources = @(
    "$fw\src\main.c", "$fw\src\protocol.c", "$fw\src\worker.c", "$fw\src\transport_spi.c", "$fw\src\transport_sdio.c",
    "$fw\hal\ti\cc3501e_hw_ti.c", "$fw\hal\ti\transport_hw_ti_spi.c", "$fw\hal\ti\transport_hw_ti_sdio.c",
    # SysConfig unity aggregates (bundle the FreeRTOS kernel + dpl) + drivers config.
    # ti_freertos_config.c ALSO provides the device startup: it #includes
    # <startup/startup_cc35xx_ticlang.c> and defines THE vector table (resetVectors
    # with the real SVC_Handler/PendSV_Handler/SysTick_Handler) + resetISR.  Do NOT
    # link a separate startup file -- doing so emits a SECOND .resetVecs whose every
    # exception slot is the IntDefaultHandler while(1) stub; that stub table lands
    # FIRST at 0x14002000 (the table the SES boots), so the FreeRTOS scheduler's first
    # SVC traps in a spin and the app never runs (root-caused 2026-06-17).
    "$out\ti_drivers_config.c", "$out\ti_freertos_config.c", "$out\ti_freertos_portable_config.c",
    # Flash-map slot symbols (vendor_image_*_slot_*_address, bl2_*, wifi_connectivity_*) that the
    # PSA-FWU lib (FWU.a) references -- generated by the SysConfig MemoryConfigurator (build/ti/memcfg).
    # Required for the psa_fwu_accept() cold-boot fix (TRM §10.3.2 MCUboot trial-image commit).
    "$out\memcfg\ti_flash_map_config.c")

# OTA-self-install validation updater: embed the signed candidate vendor image blob +
# the OTFDE flash-decryption driver that psa_fwu_start() pulls in (FlashSetOTFDE is in
# the already-linked drivers_cc35xx.a; otfde_driver.c just needs its 3 header dirs).
if ($OtaSelftest) {
    $sources += "$fw\hal\ti\cc3501e_ota_candidate.c"
    $sources += "$SdkDir\source\ti\drivers\net\wifi\wifi_platform\cc35xx\plat\otfde_driver.c"
    $inc += @("-I$SdkDir\source\ti\drivers\net\wifi\wifi_host_driver\inc_adapt",
              "-I$SdkDir\source\ti\drivers\net\wifi\wifi_host_driver\inc_common",
              "-I$SdkDir\source\ti\drivers\net\wifi\wifi_platform\cc35xx\inc_common",
              "-I$SdkDir\source\ti\drivers\xmem\flash")
}

# Wi-Fi host integration (P0-5): the OSI/glue/app sources the wifi libs reference but
# that ship as SOURCE only (not in any .a) -- wifi_stack.a lists osi_* as undefined U.
# Paths + the (above) include set come from the SDK network_terminal demo makefile.
#  - osi_dpl.c        : the full OSI -> DPL/FreeRTOS port (SyncObj/Thread/MsgQ/Timer/...)
#  - osi_filesystem.c : OSI FS/NV (connectivity-FW container access)
#  - nvocmp_cc35xx.c  : NV-over-CMP impl (NVOCMP_loadApiPtrs, which osi_filesystem.c calls)
#  - crc.c            : crc_update (nvocmp_cc35xx.c uses it for the NV page CRC)
#  - uart_term.c      : provides Report() (libs call it)
#  - syslog.c         : OSI syslog
#  - network_lwip.c   : network_stack_* (lwIP bring-up; WIFI_CONNECT/AP/GET_IP need it)
if ($WifiHostDriver) {
    $ntDir = "$SdkDir\examples\rtos\LP_EM_CC35X1\demos\network_terminal"
    $sources += "$ntDir\adaptation\osi_dpl.c"
    $sources += "$ntDir\adaptation\osi_filesystem.c"
    $sources += "$ntDir\nvocmp_cc35xx.c"
    $sources += "$ntDir\crc.c"
    $sources += "$ntDir\adaptation\uart_term.c"
    $sources += "$ntDir\adaptation\syslog.c"
    $sources += "$ntDir\network_lwip.c"
    #  - dhcpserver.c     : dhcps_start/dhcps_stop -- network_lwip.c's AP bring-up
    #                       path (network_set_up on the AP if, for WIFI_SOFTAP) starts
    #                       the DHCP server; STA-only builds never referenced it, so it
    #                       was previously dead-stripped.  Only lwIP + osi_kernel deps.
    $sources += "$ntDir\dhcpserver.c"
}

Write-Host "== Compile ($($sources.Count) sources) =="
$objs = @()
foreach ($s in $sources) {
    $o = "$out\$((Split-Path $s -Leaf)).o"
    $cout = & $tc @cflags @txdef @inc $s -o $o 2>&1
    if ($LASTEXITCODE -ne 0) { $cout | ForEach-Object { Write-Host "  $_" }; throw "compile failed: $s" }
    $objs += $o
}

Write-Host "== Linker: VENDOR-APP map (cc35xx_freertos.cmd, FLASH@0x14000000, vectors@0x14002000) =="
# *** ROOT CAUSE FIX (2026-06-17). ***  A CC35 VENDOR APP must link at FLASH base
# 0x14000000 with its interrupt vector table at 0x14002000 -- that is exactly
# where the secure boot (SES) sets VTOR and reads the reset vector.  We had been
# using cc35xx.cmd (FLASH base 0x10000000, vectors 0x10001100), which is the WRONG
# image type: the SES set VTOR=0x14002000, found no valid vectors at our 0x10001100
# table, jumped to garbage, and faulted before main() (the app never ran -> no SPI
# reply -> PING never passed).  Confirmed by dumping TI's reference vendor_app.out:
# .resetVecs@0x14002000, .text@0x14002400.  cc35xx_freertos.cmd is the linker the
# SDK's FreeRTOS vendor-app examples use; switch to it.
#
# It #includes a SysConfig MemoryConfigurator file (ti_build_linker.cmd.toolbox)
# we don't generate -- emit a minimal stub with the two symbols it needs.  The
# board cmd also uses --entry_point resetISR; our startup names it ResetISR.
$stockCmd = "$SdkDir\source\ti\boards\cc35xx\cc35xx_freertos.cmd"
$localCmd = "$out\cc3501e_vendor.cmd"
@'
/* Stub for the SysConfig MemoryConfigurator output that cc35xx_freertos.cmd
 * #includes.  CC3501E external flash = 8 MB (PY25Q64LB); no PSRAM populated. */
#define build_linker_toolbox_FLASH_SIZE 0x00800000
#define build_linker_toolbox_PSRAM_SIZE 0
'@ | Set-Content "$out\ti_build_linker.cmd.toolbox"
# Keep the stock `--entry_point resetISR` -- the SysConfig startup (in ti_freertos_config.c)
# provides resetISR + the real vector table; no separate startup is linked.
#
# *** COLD-BOOT ROOT-CAUSE FIX (2026-06-17): put the INITIAL STACK in TCM, not DRAM. ***
# The stock simple-example cmd places `.stack : > DRAM (HIGH)` (DRAM @ 0x28000000).  On a
# cold power-on reset the DRAM bank is NOT powered/ready when the SES hands the core to our
# vendor image, so the very first stack push (localProgramStart prologue / SetupTrimDevice)
# writes to dead DRAM and the image faults BEFORE Board_init -- the app never runs, POCI is
# never driven, the inter-chip PING never passes.  Warm reset "worked" only because the prior
# session left DRAM powered.  TI's SHIPPABLE vendor app (network_terminal) links __stack into
# TCM_DRAM_NON_SECURE (0x20000000), which is alive the instant the core leaves cold reset; the
# early startup then enables DRAM before the C-runtime touches .data/.bss.  Match that: move
# ONLY .stack to TCM (leave .data/.bss/.sysmem in DRAM -- they run after DRAM is up).
# (Localised, witness-confirmed: cold POCI-after-Board_init probe read 0xFFFFFFFF = faulted
# pre-Board_init; the only linker delta vs TI's cold-booting ref was the stack region.)
(Get-Content $stockCmd) | ForEach-Object {
    if ($_ -match '^\s*\.stack\s*:') { $_ -replace '>\s*DRAM\s*\(HIGH\)', '> TCM_DRAM_NON_SECURE (HIGH)' }
    else { $_ }
} | Set-Content $localCmd

Write-Host "== Link =="
if ($WifiHostDriver) {
    # FULL Wi-Fi host link set (P0-5): wifi_stack.a (Wlan_*/InitHostDriver -- the impl),
    # host_driver.a, platform.a (HIF/bus/IRQ), lwip.a (IP/DHCP), hostap.a (WPA supplicant).
    # wifi<->lwip<->hostap have CIRCULAR deps, so this set is followed by ticlang's
    # -Wl,--reread_libs (the gcc --start-group/--end-group equivalent) -- the linker
    # re-reads the archives until no new undefined symbols resolve.  Order + flag placement
    # mirror the SDK network_terminal demo makefile (FWU.a kept here for the cold-boot fix).
    #
    # Secure/crypto archives (mbedtls / psa_crypto / secure_drivers / hsmddk):
    # Wlan_Start + Wlan_RoleUp(STA) bring up the WPA/TLS supplicant path, so
    # wifi_platform_cc35xx.a<tls_mbedtls.c.obj>/<crypto_mbedtls.c.obj> reference
    # the mbedtls_ssl_*/mbedtls_x509_*/psa_* surface.  In the SDK demo these come
    # from the demo syscfg's secure/crypto SysConfig modules (its genlibs); our
    # leaner cc3501e_aen_wifi.syscfg does not enable them, so link the archives
    # explicitly inside the reread group (their CC35xx HSM-backed crypto has its
    # own circular deps).  GET_MAC (P0-6) is the first op to need them: the prior
    # Wlan_Get-only baseline read a property without the TLS path.
    $wifilibs =
        @("$SdkDir\source\third_party\lwip\lib\ticlang\lwip.a",
          "$SdkDir\source\third_party\hostap\lib\ticlang\hostap.a",
          "$SdkDir\source\ti\net\wifi_stack\lib\ticlang\wifi_stack.a",
          "$SdkDir\source\ti\drivers\net\wifi\wifi_host_driver\lib\ticlang\wifi_host_driver.a",
          "$SdkDir\source\ti\drivers\net\wifi\wifi_platform\cc35xx\lib\ticlang\wifi_platform_cc35xx.a",
          "$SdkDir\source\ti\drivers\secure\lib\ticlang\m33f\secure_drivers_cc35xx_mbedtls.a",
          "$SdkDir\source\third_party\psa_crypto\lib\ticlang\m33f\psa_crypto_cc35xx.a",
          "$SdkDir\source\third_party\mbedtls\ti\lib\ticlang\m33f\mbedtls.a",
          "$SdkDir\source\third_party\hsmddk\lib\ticlang\m33f\hsmddk_cc35xx.a",
          "$SdkDir\source\third_party\hsmddk\lib\ticlang\m33f\hsmddk_cc35xx_its.a",
          # BLE interface glue: wifi_stack.a references BleIf_EnableBLE / BleIf_SetSeed /
          # BleTransport_BleEventHandler (cme.c / driver_cc35xx.c / control_cmd_fw.c) even on
          # the Wi-Fi-only path -- the shared HIF arbitration is co-owned by the BLE interface.
          # ble_interface.a resolves them; nimble.a is NOT needed unless a nimble symbol then
          # comes up undefined (it does not for Wi-Fi-only).
          "$SdkDir\source\ti\net\ble_interface\lib\ticlang\ble_interface.a",
          "$SdkDir\source\ti\utils\FWU\lib\ticlang\FWU.a",
          '-Wl,--reread_libs')
    $link = @('-Wl,-u,_c_int00', '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16') +
            $objs +
            @("-L$SdkDir\source") + $wifilibs +
            @("$out\ti_utils_build_linker.cmd.genlibs",                                 # drivers_cc35xx.a + driverlib.a
              $localCmd,
              '-Wl,--rom_model', "-Wl,-m,$out\cc3501e-bridge.map", '-o', "$out\cc3501e-bridge.out")
} else {
    $link = @('-Wl,-u,_c_int00', '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16') +
            $objs +
            @("-L$SdkDir\source",
              "$SdkDir\source\ti\utils\FWU\lib\ticlang\FWU.a",                          # PSA-FWU (psa_fwu_accept cold-boot fix)
              "$out\ti_utils_build_linker.cmd.genlibs",                                 # drivers_cc35xx.a + driverlib.a
              $localCmd,
              '-Wl,--rom_model', "-Wl,-m,$out\cc3501e-bridge.map", '-o', "$out\cc3501e-bridge.out")
}
$lout = & $tc @link 2>&1
if ($LASTEXITCODE -ne 0) { $lout | ForEach-Object { Write-Host "  $_" }; throw "link failed" }

& "$TiclangRoot\bin\tiarmobjcopy.exe" -O ihex   "$out\cc3501e-bridge.out" "$out\cc3501e-bridge.hex"
& "$TiclangRoot\bin\tiarmobjcopy.exe" -O binary "$out\cc3501e-bridge.out" "$out\cc3501e-bridge.bin"
& "$TiclangRoot\bin\tiarmsize.exe" "$out\cc3501e-bridge.out"
Write-Host "== Done: $out\cc3501e-bridge.{out,hex,bin} =="
