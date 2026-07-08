# SimpleLink CC33xx SDK — vendor wrapper

This directory is the **alp-sdk side of the TI SimpleLink CC33xx SDK
boundary** for the `cc3501e-bridge` firmware's production (`ti`) build.

alp-sdk does **not** redistribute TI's SDK. Only the thin, committed
wrapper lives here:

| Path | Tracked? | Purpose |
|------|----------|---------|
| `README.md` (this file) | yes | acquisition + integration runbook |
| `CMakeLists.txt` | yes | exports the `simplelink_cc33xx` target the `ti` build links |
| `sdk/` | **no** (gitignored) | where the extracted SDK is dropped, if you vendor it in-tree |

The top-level firmware CMake links this:
`firmware/cc3501e/CMakeLists.txt` →
`add_subdirectory(vendor/simplelink-cc33xx)` +
`target_link_libraries(cc3501e-bridge PRIVATE simplelink_cc33xx)`
(only when `-DCC3501E_HAL_BACKEND=ti`). The stub / CI path never
descends here.

## What to obtain (bench machine, one-time)

Reference the vendor's public product pages — alp-sdk ships none of these:

| Tool | Role | Product page |
|------|------|--------------|
| **TI Arm Clang** (`tiarmclang`) | the compiler the SDK is built + validated with; drives `toolchain/ticlang.cmake` | ti.com → `ARM-CGT-CLANG` |
| **SysConfig** | generates `ti_drivers_config.{c,h}` (the board file: `CONFIG_SPI_0`) | ti.com → `SYSCONFIG` |
| **SimpleLink Wi-Fi SDK** (CC35xxE family) | the `sl_*` host, TI Drivers, BLE 5.4 host, FreeRTOS, LwIP — the CC3501E runs on this | ti.com → `SIMPLELINK-WIFI-SDK` (myTI login + export agreement) |

Pin the exact versions you install here so production images are
reproducible:

```
simplelink-wifi   : 10.10.01.08   (extracted: Desktop/ti_simplelink_sdk/simplelink_wifi_sdk_10_10_01_08)
tiarmclang        : 5.1.1.LTS      (installed: C:/ti/ti-cgt-armllvm-5.1.1.LTS/ti-cgt-armllvm_5.1.1.LTS)
                    NB: this SDK's imports.mak pins ticlang 4.0.4.LTS -- if 5.1.1 mis-compiles, drop to 4.0.4.
sysconfig         : 1.26.3 REQUIRED (imports.mak) -- ships with CCS 20.4.1 or as standalone SysConfig.
gcc-arm-none-eabi : 13.3 present    (SDK also supports GCC; imports.mak default 12.3) -- fallback toolchain.

# SDK integration (grounded 2026-06-16):
#  - The SDK is itself a CMake project (project(coresdk), add_subdirectory-able);
#    keys off env TICLANG_ARMCOMPILER / GCC_ARMCOMPILER + add_compile_definitions(CC35XX).
#    Lib targets incl. device_cc35xx, drivers, wifi_public, freertos_kernel, tiutils.
#  - Wi-Fi host driver: source/ti/drivers/net/wifi/wifi_host_driver (+ wifi_platform/cc35xx).
#    BLE: source/ti/net/ble_interface + source/third_party/nimble.
#  - CAVEAT: the v0.1 ti HAL (hal/ti/cc3501e_hw_ti.c) includes
#    <ti/drivers/net/wifi/simplelink.h> + calls sl_Start/sl_NetCfgGet -- the classic
#    CC32xx host API.  This SDK (CC35xx) reorganizes that surface; the header is NOT at
#    that path.  v0.1 fix: GET_MAC returns NOT_READY with NO wifi-host dependency; the
#    real MAC read + Wi-Fi land in v0.2.  Board file basis: examples/rtos/LP_EM_CC35X1/demos/*.syscfg.
```

## Point the build at the SDK

Two options (pick one):

1. **External (recommended — no multi-GB copy in-tree):** leave the SDK
   installed wherever TI's installer put it and pass its path at
   configure time:
   ```
   -DSIMPLELINK_SDK_DIR=<path-to-extracted-sdk>
   ```
2. **In-tree:** copy/symlink the extracted SDK into `./sdk/` (gitignored).

## SysConfig board file (CONFIG_SPI_0)

The `ti` SPI-slave backend (`../../hal/ti/transport_hw_ti_spi.c`) opens
`CONFIG_SPI_0`. Generate the board file with SysConfig for the E1M-AEN
module, configuring the inter-chip SPI instance per
`metadata/e1m_modules/aen/inter-chip.tsv`:

| SysConfig setting | Value | Source |
|-------------------|-------|--------|
| SPI instance | `CONFIG_SPI_0` | the inter-chip link |
| Mode | `SPI_SLAVE` | Alif is master |
| Frame format | mode 0 (`SPI_POL0_PHA0`) | host driver + chip manifest |
| SCLK | `GPIO_27` | inter-chip.tsv |
| MOSI | `GPIO_28` | inter-chip.tsv |
| MISO | `GPIO_29` | inter-chip.tsv |
| CS | `GPIO_16` CSN, framed by Alif hardware SS0 | current E1M-AEN bridge |
| READY | `GPIO_17` | host gates reply phases on READY |

The generated `ti_drivers_config.{c,h}` becomes part of
`SIMPLELINK_CC33XX_SOURCES` / `SIMPLELINK_CC33XX_INCLUDES` (see
`CMakeLists.txt`).

## Build (bench)

```
cmake -B build/ti -S firmware/cc3501e \
  -DCMAKE_TOOLCHAIN_FILE=firmware/cc3501e/toolchain/ticlang.cmake \
  -DCC3501E_HAL_BACKEND=ti \
  -DSIMPLELINK_SDK_DIR=<path-to-extracted-sdk>
cmake --build build/ti          # -> build/ti/cc3501e-bridge.hex / .bin
```

Set `TICLANG_ROOT` (env) to the `tiarmclang` install prefix if its
binaries are not on `PATH` (see `toolchain/ticlang.cmake`).

Flash the resulting image to the CC3501E over SWD/J-Link (its dedicated
probe); the host-side bring-up then drives PING/GET_VERSION from the Alif
over the inter-chip SPI1.
