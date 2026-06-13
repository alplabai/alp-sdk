@page firmware_cc3501e_index CC3501E bridge firmware

# cc3501e-bridge

Firmware that runs on the **TI CC3501E** Wi-Fi 6 + BLE 5.4 coprocessor
populated on the E1M-AEN module family
(`E1M-AEN301/401/501/601/701/801`).  It is the SPI-slave (or, on
SDIO-routed boards, SDIO-slave) parser that fronts TI's SimpleLink
Wi-Fi + BLE stacks for the Alif Ensemble host.

This tree is a **separate compile artifact** with its own toolchain (TI
`ticlang` for the CC3501E Cortex-M33) and its own flash binary.  It is
**not** linked into the Zephyr-side alp-sdk library; the matching
host-side driver lives at [`chips/cc3501e/`](../../chips/cc3501e/) and
the wire protocol at
[`include/alp/protocol/cc3501e.h`](../../include/alp/protocol/cc3501e.h).

It is **embedded in alp-sdk** -- not split into a separate repo -- for
the same reasons the [`gd32-bridge`](../gd32-bridge/) is: the wire
protocol stays single-source (the firmware `#include`s the canonical
header directly, no mirror to drift), and an opcode change moves the
host driver, this firmware, and the wire-vector tests in **one commit**.
See [ADR 0015](../../docs/adr/0015-cc3501e-firmware-embedded.md).

## Tree layout

```
firmware/cc3501e/
├── CMakeLists.txt          ← backend (stub|ti) + transport (spi|sdio) selection
├── README.md               ← this file
├── DESIGN.md               ← v0.1 scope, wire-reply contract, reconciliation items
├── firmware-version.txt    ← firmware RELEASE semver (own axis)
├── protocol-version.txt    ← wire-protocol version expected (= ALP_CC3501E_PROTOCOL_VERSION)
├── flash.py                ← consumer-side prebuilt-binary flasher (stub until first binary)
├── prebuilt/               ← signed binaries shipped with each alp-sdk release
├── toolchain/              ← arm-none-eabi (stub/CI smoke) + ticlang (bench/production)
├── src/
│   ├── main.c              ← entry: hw init → selected transport init → WFI loop
│   ├── protocol.{c,h}      ← shared command-handler table + transport-agnostic framing
│   ├── transport.h         ← SPI + SDIO seam declarations
│   ├── transport_spi.c     ← SPI-slave staging (DEFAULT link)        — silicon-free
│   └── transport_sdio.c    ← SDIO-slave staging (OPTIONAL link)      — silicon-free
├── hal/
│   ├── cc3501e_hw.h        ← HAL contract (init, tick, get_mac, reset)
│   ├── cc3501e_hw_stub.c   ← hardware-free backend (host tests + CI compile smoke)
│   └── ti/                 ← real TI SimpleLink / driverlib backend (bench build)
└── tests/
    ├── gen_protocol_vectors.py  ← canonical wire vectors (regenerate + --check)
    └── protocol_vectors.txt
```

Every transport calls the same `protocol_build_reply()` /
`protocol_dispatch()` in `src/protocol.c` -- one framing format, one
command set, one set of reply codes; only the byte-level transport
differs.

## Selectable host-control transport

The Alif↔CC3501E control link is **customer-selectable**:

| Transport | Role | Availability |
|-----------|------|--------------|
| **SPI1 slave** | **DEFAULT** + always-available baseline/fallback | Always (CC3501E GPIO_27/28/29) |
| **SDIO slave** | OPTIONAL, higher throughput for Wi-Fi data | Only when the board routes the Alif's **single** SDIO controller to the CC3501E -- **mutually exclusive with a micro-SD card** (CC3501E GPIO_3/4/5/6/10/11) |

Because the Alif Ensemble has one SDIO controller shared at board level
with the SD slot, SDIO is available to the CC3501E only on boards
without an SD card; when an SD card is used, **SDIO is blocked and the
link falls back to SPI**.  Pick the transport at build time
(`-DCC3501E_CONTROL_TRANSPORT=spi|sdio`, default `spi`); in a studio
build the choice is sourced from the customer `board.yaml`.  Both
transports always compile; the selector only chooses which one `main()`
starts.

## Build

The CMake build runs **outside** the Zephyr build (the Alif side's
`west build` does not descend here).  Two backends:

```bash
# Host-side / CI compile smoke -- silicon-free, no TI SDK needed:
cmake -B build/stub -S firmware/cc3501e \
  -DCMAKE_TOOLCHAIN_FILE=firmware/cc3501e/toolchain/arm-none-eabi.cmake \
  -DCC3501E_HAL_BACKEND=stub
cmake --build build/stub

# Production image (bench) -- needs TI ticlang + the SimpleLink CC33xx SDK:
cmake -B build/ti -S firmware/cc3501e \
  -DCMAKE_TOOLCHAIN_FILE=firmware/cc3501e/toolchain/ticlang.cmake \
  -DCC3501E_HAL_BACKEND=ti
cmake --build build/ti
```

The `ti` backend vendors TI's **BSD-3-licensed** SimpleLink CC33xx SDK
as a submodule under `vendor/simplelink-cc33xx/` (obtained from TI;
alp-sdk does not redistribute it) and builds with `ticlang` (TI's Arm
LLVM compiler -- pin the exact version on the bench for reproducible
images).  CI builds only the `stub` backend (the silicon-free layer is
toolchain-agnostic portable C; that catches compile + framing errors
without the ~GB vendor SDK); the wire behaviour is additionally pinned
by the native test `tests/zephyr/cc3501e_bridge_transport/` under the
twister gate.

## v0.x roadmap

v0.1 ("bring-up") implements the META command group only -- PING,
GET_VERSION, GET_MAC, RESET -- enough to prove the link is alive and
version-compatible.  Everything else (Wi-Fi, BLE, GPIO proxy,
camera-enable, power, diagnostics) is rejected with
`ALP_CC3501E_RESP_ERR_INVALID` and lands in v0.2+.  The full roadmap is
in [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md)
"v0.x roadmap".

## Source-of-truth contract

| Contract | Defined in |
|----------|------------|
| Wire protocol (commands, frames, events) | [`include/alp/protocol/cc3501e.h`](../../include/alp/protocol/cc3501e.h) (included directly -- no mirror) |
| Bridge architecture + GPIO behaviour contract | [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md) |
| Inter-chip wiring (SPI1 / SDIO / control lines) | [`metadata/e1m_modules/aen/inter-chip.tsv`](../../metadata/e1m_modules/aen/inter-chip.tsv) |
| E1M ↔ CC3501E pad routing | [`metadata/e1m_modules/aen/from-cc3501e.tsv`](../../metadata/e1m_modules/aen/from-cc3501e.tsv) |
| v0.1 scope + wire-reply contract + reconciliation items | [`DESIGN.md`](DESIGN.md) |

## Versioning

Three independent axes (same model as the gd32-bridge):

| Axis | Where | Bumps when |
|------|-------|-----------|
| Firmware release | `firmware-version.txt` | each firmware release -- names the tag + the `prebuilt/cc3501e-vX.Y.Z.bin` blob |
| Wire protocol | `ALP_CC3501E_PROTOCOL_VERSION` (`<alp/protocol/cc3501e.h>`) + `protocol-version.txt` | the wire format changes; the host refuses a mismatched version via GET_VERSION |
| Build / signature | the signed binary's `.sha256` in `prebuilt/` | every build |

## Flashing (consumer-side)

The CC3501E ships **pre-flashed by Alp**; for normal use the customer
flashes nothing.  A version-pinned prebuilt blob also lives at
`prebuilt/cc3501e-vX.Y.Z.bin` for field re-flash via
[`flash.py`](flash.py) (which relays the image to the CC3501E over the
inter-chip link).  `flash.py` and `prebuilt/` are populated when the
first production binary is built + signed on the bench.

## Status

| Milestone | Status |
|-----------|--------|
| Wire-protocol header + host driver | ✅ landed (`include/alp/protocol/`, `chips/cc3501e/`) |
| Firmware tree (embedded) | ✅ this tree |
| v0.1 META group (PING / GET_VERSION / GET_MAC / RESET) | ✅ silicon-free + stub backend; native test green |
| TI backend: SPI-slave + lifecycle (`hal/ti/`) | ✅ implemented against TI Drivers (`SPI_open` SPI_SLAVE callback) + SimpleLink (`sl_Start`/`sl_NetCfgGet`) + CMSIS reset. Compiles on the bench against the SimpleLink CC35xx SDK + a SysConfig board file. Bench-confirm: the SysConfig anchors (`CONFIG_SPI_0`, `CONFIG_GPIO_CC3501E_HOST_READY`) + the reply-timing handshake (needs a CC3501E→Alif READY line — see DESIGN.md). |
| TI backend: SDIO-slave (`hal/ti/transport_hw_ti_sdio.c`) | 🟡 frame glue complete; the SDIO-**device** register bring-up needs SWRU626 §21 (no public SDK SDIO-device driver). Off the v0.1 critical path — SPI is the default. |
| `flash.py` real flashing | 🔮 blocked on TI's `cc3501e-flasher` CLI (not public yet); manual SWD/J-Link is the interim bench path |
| `prebuilt/` populated | 🔮 when the first bench binary is built/signed (first board: E1M-AEN801) |
| Wi-Fi / BLE / GPIO-proxy groups (v0.2+) | 🔮 roadmap |
