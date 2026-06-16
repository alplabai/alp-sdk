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
    [string]$Transport    = "spi"   # spi | sdio
)

$ErrorActionPreference = 'Stop'
$fw   = Split-Path $PSScriptRoot -Parent          # firmware/cc3501e
$repo = (Resolve-Path "$fw\..\..").Path           # repo root
$out  = "$fw\build\ti"
$tc   = "$TiclangRoot\bin\tiarmclang.exe"
New-Item -ItemType Directory -Force $out | Out-Null

Write-Host "== SysConfig: generate the board file (CONFIG_SPI_0) =="
& $SysconfigCli --product "$SdkDir\.metadata\product.json" --compiler ticlang `
    --output $out "$PSScriptRoot\cc3501e_aen.syscfg"
if ($LASTEXITCODE -ne 0) { throw "SysConfig failed" }

$inc = @(
    "-I$fw\src", "-I$fw\hal", "-I$repo\include", "-I$out",
    "-I$SdkDir\source", "-I$SdkDir\kernel\freertos", "-I$SdkDir\source\ti\posix\ticlang",
    "-I$SdkDir\source\third_party\freertos\include",
    "-I$SdkDir\source\third_party\freertos\portable\GCC\ARM_CM33_NTZ\non_secure")
$cflags = @('-c', '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16',
            '-DDeviceFamily_CC35XX', '-DCC35XX', '-DCC3501E_RTOS_FREERTOS', '-Oz',
            '-ffunction-sections', '-fdata-sections', '-Wall')

$txdef = if ($Transport -eq 'sdio') { @('-DCC3501E_CONTROL_TRANSPORT_SDIO=1') } else { @() }

# App + the silicon-free layer + the ti HAL.
$sources = @(
    "$fw\src\main.c", "$fw\src\protocol.c", "$fw\src\transport_spi.c", "$fw\src\transport_sdio.c",
    "$fw\hal\ti\cc3501e_hw_ti.c", "$fw\hal\ti\transport_hw_ti_spi.c", "$fw\hal\ti\transport_hw_ti_sdio.c",
    # SysConfig unity aggregates (bundle the FreeRTOS kernel + dpl) + drivers config.
    "$out\ti_drivers_config.c", "$out\ti_freertos_config.c", "$out\ti_freertos_portable_config.c",
    # Device startup (vector table + ResetISR).
    "$SdkDir\source\ti\devices\cc35xx\startup_files\startup_ticlang.c")

Write-Host "== Compile ($($sources.Count) sources) =="
$objs = @()
foreach ($s in $sources) {
    $o = "$out\$((Split-Path $s -Leaf)).o"
    & $tc @cflags @txdef @inc $s -o $o
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
    $objs += $o
}

Write-Host "== Link =="
$link = @('-Wl,-u,_c_int00', '-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16') +
        $objs +
        @("-L$SdkDir\source",
          "$out\ti_utils_build_linker.cmd.genlibs",                                  # drivers_cc35xx.a + driverlib.a
          "$SdkDir\source\ti\devices\cc35xx\linker_files\cc35xx.cmd",
          '-Wl,--rom_model', "-Wl,-m,$out\cc3501e-bridge.map", '-o', "$out\cc3501e-bridge.out")
& $tc @link
if ($LASTEXITCODE -ne 0) { throw "link failed" }

& "$TiclangRoot\bin\tiarmobjcopy.exe" -O ihex   "$out\cc3501e-bridge.out" "$out\cc3501e-bridge.hex"
& "$TiclangRoot\bin\tiarmobjcopy.exe" -O binary "$out\cc3501e-bridge.out" "$out\cc3501e-bridge.bin"
& "$TiclangRoot\bin\tiarmsize.exe" "$out\cc3501e-bridge.out"
Write-Host "== Done: $out\cc3501e-bridge.{out,hex,bin} =="
