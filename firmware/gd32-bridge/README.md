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
cd gd32-bridge
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
cmake --build build
```

The build emits `build/gd32-bridge.elf` + `.hex` + `.bin`.  Flashing
in the development case is done with an external SWD probe on
`GD32_SWDIO` / `GD32_SWCLK` (J-Link, ST-Link, OpenOCD).  Field
upgrades from the V2N host once the application-bootloader path
lands are documented in [`../docs/gd32-bridge-protocol.md`](../docs/gd32-bridge-protocol.md) §10.

> **Status (2026-05-12):** The toolchain config, HAL shim, and
> per-feature handler bodies that need the GigaDevice firmware
> library are scaffolded but **not yet implemented**.  PING and
> GET_VERSION are wired end-to-end with no HW dependency so the
> protocol round-trip can be smoke-tested against the host driver
> over a logic analyser stub before the GigaDevice library lands.

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
