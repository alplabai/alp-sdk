# GD32 bridge — firmware tree overview

The **GD32 bridge** is the firmware that runs on the GigaDevice
GD32G553MEY7TR companion MCU on every E1M-X V2N / V2N-M1 SoM.  It
gives the Renesas RZ/V2N host a uniform on-module supervisor surface
(GPIO fan-out, eight PWM channels, the ADC bank that doesn't fit on
the Renesas pinmux, the DA9292 INT/TW fault-pin forward, …) over
either of two parallel buses.

The **wire protocol** is specified in
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md); this doc
covers the **firmware-tree** side -- where the source lives, how to
build it, how to flash it, and what state the implementation is in.

> **Pre-flashed by ALP; rebuild is optional and fully open.** The
> GD32G553 ships flashed by ALP with the bridge firmware, so for normal
> use the customer does nothing — the Renesas host talks to a working
> supervisor out of the box.  Like the CC3501E bridge, the GD32 firmware is
> **open**: the source lives in this repo (`firmware/gd32-bridge/`) and
> the GigaDevice library
> is a public submodule, so rebuilding or customizing needs no gated
> download — see **Build** below.

## At a glance

| Aspect              | Today (2026-06-04)                                                                |
|---------------------|-----------------------------------------------------------------------------------|
| Firmware tree       | [`firmware/gd32-bridge/`](../firmware/gd32-bridge/)                                                 |
| Toolchain           | Arm GNU Toolchain (`arm-none-eabi-gcc`), Cortex-M33 + thumb                       |
| Build system        | CMake (separate from the Zephyr-side `west build`)                                |
| HAL                 | Stub default; `BRIDGE_HAL_BACKEND=gd32` consumes the GigaDevice firmware library via the [`alplabai/gd32g5x3-firmware-library`](https://github.com/alplabai/gd32g5x3-firmware-library) submodule at `vendors/gd32_firmware_library/upstream/` (run `git submodule update --init` once after cloning) |
| Protocol coverage   | `PING`, `GET_VERSION`, `GET_BUILD_ID` working end-to-end without HW dependency    |
| Transport coverage  | SPI1 slave (25 MHz full-DMA, silicon-validated) + I2C0 slave in `hal/transport_hw_gd32.c` (gd32 backend) |
| Datasheet           | GD32G553 datasheet + user manual (held in the vendor datasheet) |
| Flash size on chip  | 512 KB (per datasheet)                                                            |
| RAM size on chip    | 128 KB                                                                            |

## Versioning

Three independent version axes — track them separately:

| Axis | Where | Bumps when |
|------|-------|-----------|
| **Firmware release** | `firmware-version.txt` (semver, baked in via CMake) | each firmware release — names the tag + prebuilt blob; the device surfaces it through `GET_BUILD_ID` as `<ver>+<sha>` |
| **Wire protocol** | `PROTOCOL_VERSION_*` in `src/protocol.h` (host: `<alp/chips/gd32g553.h>`) | the wire format changes; `GET_VERSION` returns it and the host refuses a mismatched MAJOR |
| **Build-id** | git short-SHA in the `GET_BUILD_ID` reply | every build — pins the exact source behind a release |

A firmware release can ship without a protocol bump, and vice-versa.

## Build

```bash
cd firmware/gd32-bridge
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
cmake --build build
```

Output: `build/gd32-bridge.elf`, `.hex`, `.bin`.

* `BRIDGE_HAL_BACKEND=stub` (default) -- builds without the
  GigaDevice firmware library; every HW-touching opcode reports
  `BRIDGE_HW_ERR_NOTIMPL` which the protocol layer maps to wire
  `STATUS_IO`.  Useful for smoke-testing the protocol round-trip in
  a hardware-less unit-test environment.
* `BRIDGE_HAL_BACKEND=gd32` -- builds against
  [`vendors/gd32_firmware_library/`](../vendors/gd32_firmware_library/).
  The wrapper consumes the GigaDevice **GD32G5x3 Firmware Library**
  via a git submodule pointing at
  [alplabai/gd32g5x3-firmware-library](https://github.com/alplabai/gd32g5x3-firmware-library)
  (a verbatim mirror of v1.5.0 under SLA-GD0001 v1.1).  Run
  `git submodule update --init --recursive vendors/gd32_firmware_library/upstream`
  once after cloning, then the bridge build picks it up
  automatically.  See
  [`vendors/gd32_firmware_library/README.md`](../vendors/gd32_firmware_library/README.md)
  for the licence-redistribution constraints + the version-bump procedure.

## Source layout

```
firmware/gd32-bridge/
├── CMakeLists.txt                   (default monolithic build; -DBRIDGE_OTA_PARTITIONED=ON
│                                     emits bootloader + slot-A/B apps instead)
├── README.md
├── toolchain/
│   ├── arm-none-eabi.cmake          (Arm GNU Toolchain)
│   ├── gd32g553_flash.ld            (monolithic full-flash linker)
│   ├── gd32g553_bootloader.ld       (32 KB Path-A bootloader)
│   └── gd32g553_app_slot.ld.in      (slot-relocated app template; .ramfunc in RAM)
├── hal/
│   ├── bridge_hw.h                  (HAL surface consumed by protocol.c)
│   ├── bridge_hw_stub.c             (host-test backend, ops return NOTIMPL)
│   ├── gd32/                        (real GigaDevice peripheral HAL, one TU
│   │                                 per peripheral: init.c gpio.c trng.c
│   │                                 tmu.c vref.c adc.c adc_stream.c dac.c
│   │                                 qenc.c pwm.c pwm_capture.c counter.c
│   │                                 timer_sync.c power.c + gd32_common.h)
│   ├── transport_hw_gd32.c          (SPI1 + I2C0 slave silicon bring-up, full-DMA SPI)
│   └── fmc_ota.c                    (RAM-resident dual-bank FMC erase/program)
├── src/
│   ├── main.c                       (entry point, WFI loop)
│   ├── protocol.c                   (shared command-handler table)
│   ├── protocol.h
│   ├── transport_spi.c              (SPI-slave receive + reply staging)
│   ├── transport_i2c.c              (I2C-slave receive + reply staging)
│   ├── ota.c / ota.h / ota_layout.h (OTA Path-A state machine + A/B metadata)
│   ├── crc32.c / crc32.h            (IEEE CRC-32 shared by app + bootloader)
│   ├── boot/boot_main.c             (bootloader: slot pick + CRC validate + jump)
│   └── bootloader/                  (0xF0..0xFF dispatch into ota.c)
├── tools/
│   └── gen_ota_metadata.py          (factory A/B metadata record for first-flash)
└── tests/
    ├── gen_protocol_vectors.py      (CRC + wire-vector generator)
    └── protocol_vectors.txt         (CRC + wire vectors shared with host)
```

The single design rule that makes this tree work:
**both transports call the same `protocol_dispatch()`** in
`src/protocol.c`.  Adding an opcode is therefore a one-place
change.

## Flashing

| Method                                | Status today      | Notes                                                                                                                                  |
|---------------------------------------|-------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| External SWD probe (J-Link, ST-Link)  | **Supported.**    | SWDIO + SWCLK accessible on the V2N module's programming header.                                                                       |
| In-system upgrade over SPI / I2C      | **Implemented, gated — silicon-validated 2026-06-04.** | Application-bootloader path; the `0xF0..0xFF` opcodes route through `src/bootloader/` into the OTA state machine in [`src/ota.c`](../firmware/gd32-bridge/src/ota.c) (FMC backend `hal/fmc_ota.c`). Destructive flashing is armed only with `-DBRIDGE_OTA_PARTITIONED`; default builds reply `STATUS_NOSUPPORT` (can't brick the running image). The armed build emits the partitioned set (32 KB bootloader + slot-A/B apps); first-flash also needs the factory metadata record from [`tools/gen_ota_metadata.py`](../firmware/gd32-bridge/tools/gen_ota_metadata.py) at `0x08008000`. Validated end-to-end on the bench: stream → verify → commit → boot new slot → rollback (protocol v0.6). See [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10 Path A. |
| Host-driven SWD bit-bang from V2N     | **Scaffolded.**   | Renesas-side software SWD controller drives `GD32_SWDIO` + `GD32_SWCLK` (routed back to V2N pads per the 2026-05-12 HW decision); universal recovery + factory first-flash.  Driver lives at [`chips/gd32_swd/`](../chips/gd32_swd/) (`driver_status: partial` until exercised on real silicon).  See [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10 Path B. |

## Cross-link

* Wire spec: [`gd32-bridge-protocol.md`](gd32-bridge-protocol.md).
* Host-side driver header: [`<alp/chips/gd32g553.h>`](../include/alp/chips/gd32g553.h).
* Host-side driver source: [`chips/gd32g553/gd32g553.c`](../chips/gd32g553/gd32g553.c).
* GD32 pad allocation map: [`metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`](../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv).
