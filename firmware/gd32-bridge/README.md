# gd32-bridge

Firmware that runs on the **GigaDevice GD32G553MEY7TR** supervisor
MCU on the E1M-X V2N / V2N-M1 SoMs.  Serves the Renesas RZ/V2N host
over the **hybrid SPI + I2C bridge** documented in
[`../docs/gd32-bridge-protocol.md`](../docs/gd32-bridge-protocol.md).

This tree is a **separate compile artifact** with its own toolchain
(ARM-GCC for Cortex-M33) and its own flash binary.  It is **not**
linked into the Zephyr-side `alp-sdk` library; the matching
host-side driver lives at [`chips/gd32g553/`](../chips/gd32g553/).

## Tree layout

```
firmware/gd32-bridge/
├── CMakeLists.txt          ← top-level build entry (host-built, cross-compiled)
├── README.md               ← this file
├── toolchain/              ← ARM-GCC + linker script for GD32G553MEY7TR
├── hal/                    ← thin shims around the GigaDevice firmware library
├── src/
│   ├── main.c              ← startup + dispatch loop
│   ├── protocol.c          ← shared command-handler table  ← single source
│   ├── protocol.h          ← internal header
│   ├── transport_spi.c     ← SPI-slave receive + reply staging
│   ├── transport_i2c.c     ← I2C-slave receive + reply staging
│   └── …                   ← per-feature handlers (pwm.c, gpio.c, adc.c, …)
└── tests/
    └── protocol_vectors.txt  ← canonical CRC + wire vectors (shared with host tests)
```

The transport layer is **per-bus**, but every transport calls the
**same** `protocol_dispatch()` entry point so that adding an opcode
is a one-place change — never "fork the protocol".  This mirrors
the project-memory rule:

> Same command frame on both transports … one framing format, one
> command set, one set of reply codes; only the transport layer
> differs.

## Build

The CMake build runs **outside** the Zephyr build (the Renesas
side's `west build` does not descend here).  Invoke directly:

```bash
cd firmware/gd32-bridge
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
cmake --build build
```

The default build emits the monolithic `build/gd32-bridge.elf` + `.hex`
+ `.bin` (OTA inert — the whole `0xF0..0xFF` range answers
`STATUS_NOSUPPORT`, so the image cannot brick itself).

**`-DBRIDGE_OTA_PARTITIONED=ON`** (requires `BRIDGE_HAL_BACKEND=gd32`)
arms the in-system upgrade path and emits the partitioned set instead:
`gd32-bootloader` (32 KB at flash base), `gd32-bridge-slot-a` and
`gd32-bridge-slot-b` (the app linked per A/B slot, `.ramfunc` FMC loop
in RAM, `SCB->VTOR` relocated).  First-flash of a partitioned part also
needs the factory A/B metadata record —
[`tools/gen_ota_metadata.py`](tools/gen_ota_metadata.py) generates it
(flash to `0x08008000`); without it the bootloader idles in its
recovery loop.  The full Path-A wire contract is
[`../docs/gd32-bridge-protocol.md`](../docs/gd32-bridge-protocol.md) §10.

Development flashing uses an external SWD probe on `GD32_SWDIO` /
`GD32_SWCLK` (J-Link, ST-Link, OpenOCD).

> **Status:** Both backends build clean.  The gd32 backend drives the
> real peripheral HAL (per-peripheral TUs under `hal/gd32/`), the
> **SPI1 + I2C0 slave transports** (`hal/transport_hw_gd32.c`), and
> the **OTA Path-A state machine** (`src/ota.c` + `hal/fmc_ota.c` —
> silicon-validated end-to-end 2026-06-04; armed only with
> `BRIDGE_OTA_PARTITIONED`).  The fw v0.2.3–v0.2.7 campaign cleared
> the soak-quarantined HAL defects (`pwm_capture`, `adc_stream`,
> `qenc`, `tmu` — silicon-validated; the analog subsystem additionally
> needed the v0.2.6 internal-VREF bring-up).  Remaining gap: ADC
> DSP-chain runtime dispatch degrades to error statuses.  The stub
> backend stays HW-free for host protocol round-trip tests.

## Protocol majorset

The firmware ships with a build-time `PROTOCOL_VERSION_MAJOR`
constant in [`src/protocol.c`](src/protocol.c).  Bumping the major
breaks every host that has not been rebuilt against the matching
[`<alp/chips/gd32g553.h>`](../include/alp/chips/gd32g553.h) -- treat
it as a wire-incompatible change and stage carefully.

## GPIO + PWM channel maps

The wire-side **logical** ids that the protocol uses do **not**
match GD32 silicon pad indices.  The mapping lives in
[`src/protocol.c`](src/protocol.c) in the `pwm_channel_map[]` /
`gpio_pad_map[]` tables, sourced from
[`../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`](../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv).
Host code reaches a channel by its logical id; the firmware
translates internally.

## Cross-link

* Protocol wire spec: [`../docs/gd32-bridge-protocol.md`](../docs/gd32-bridge-protocol.md).
* Host-side driver public API: [`../include/alp/chips/gd32g553.h`](../include/alp/chips/gd32g553.h).
* Host-side driver implementation: [`../chips/gd32g553/gd32g553.c`](../chips/gd32g553/gd32g553.c).
* GD32 pad map: [`../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`](../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv).
* Hybrid bridge memory note: [`../memory/project_gd32_bridge_hybrid_spi_i2c.md`](../memory/project_gd32_bridge_hybrid_spi_i2c.md).
