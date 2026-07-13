# cc3501e-bridge firmware — design notes

Scope, the wire framing, and the bench bring-up plan.  The authoritative
wire contract is
[`include/alp/protocol/cc3501e.h`](../../include/alp/protocol/cc3501e.h);
this file records the firmware-side decisions and the host/firmware
framing they share.

## v0.1 scope ("bring-up")

The META command group only, enough to prove the link is alive and
version-compatible:

| Opcode | Behaviour |
|--------|-----------|
| `PING` (0x00) | empty reply data → `RESP_OK`; the liveness signal |
| `GET_VERSION` (0x01) | reply data = `ALP_CC3501E_PROTOCOL_VERSION` (u16 LE) |
| `GET_MAC` (0x03) | reply data = 6-byte factory MAC (HAL); stub → `RESP_ERR_NOT_READY` |
| `RESET` (0x02) | ack `RESP_OK`, then HAL reboots after the ack is drained |

Every other opcode (Wi-Fi, BLE, GPIO proxy, camera, power, diagnostics)
returns `ALP_CC3501E_RESP_ERR_INVALID`, per the protocol header's
contract that v1 firmware rejects opcodes it does not implement.  Those
groups land in v0.2+ and route to TI's SimpleLink Wi-Fi / BLE APIs
through the `hal/ti/` backend.

## Wire framing -- hardware-SS0 phases + READY (current HW rev)

Both sides build the same frame: a 4-byte LE header
`[cmd | flags | payload_len(LE16)]` + payload.  The reply echoes the
request `cmd`, uses `flags = 0` (solicited; async events in v0.2+ set
`ALP_CC3501E_FLAG_ASYNC_EVENT`), and its payload is `[status][data...]`
— the response status (`ALP_CC3501E_RESP_*`) is the first payload byte,
per the header.  Framing + dispatch is centralised in
`protocol_build_reply()` so SPI and SDIO are byte-identical.

The **current E1M-AEN rev wires SCLK/MOSI/MISO plus a real hardware
chip-select**: the Alif side muxes `P14_7` as `SPI1_SS0_C`, and the
dwc-ssi master asserts/deasserts SS0 around each SPI transfer.  READY
(`P2_6` on the Alif side) gates each reply phase so the host does not
clock a slave that has not re-armed yet.  A request/reply is therefore
clocked as **four SS0-framed protocol phases**, each side deriving the
next length from a header it already exchanged:

| # | master clocks | direction | length |
|---|---------------|-----------|--------|
| 1 | request header | MOSI | 4 |
| 2 | request payload | MOSI | `payload_len` (from #1) |
| 3 | reply header | MISO | 4 |
| 4 | reply payload | MISO | reply `payload_len` (from #3) = status + data |

The host waits for READY before the reply header and reply payload
phases.  Firmware side: `hal/ti/transport_hw_ti_spi.c` (a `SPI_SLAVE` +
`SPI_MODE_CALLBACK` state machine that replays the captured frame through
the silicon-free byte seams and advances on transfer completion).  Host
side: `chips/cc3501e/cc3501e.c` `cc3501e_request()` (matching four-phase
sequence + `resp_to_status()` on `payload[0]`).  Host + firmware are
reconciled to each other and to the header, and this hardware-SS0 bridge
has been bench-validated on E1M-AEN801.

GET_VERSION returns the *protocol* version (the host's compat gate),
not the firmware *release* version — the diag-struct comment that
implied otherwise was a header doc slip (now fixed); the release version
is surfaced via `GET_DIAG_INFO.fw_version` in v2.

## Three version numbers (keep them straight)

The bridge carries **three** independent version numbers; each gates a
different thing and they must not be conflated:

| Version | Source of truth | Surfaced by | Gates |
|---|---|---|---|
| **App SemVer** | `firmware-version.txt` (e.g. `0.2.0`) | `GET_DIAG_INFO.fw_version` (u16) | firmware release identity — human-facing "what's running" |
| **Wire protocol version** | `ALP_CC3501E_PROTOCOL_VERSION` in `<alp/protocol/cc3501e.h>` (currently `3`) | `GET_VERSION` (0x01) | host↔firmware wire compatibility (host refuses a mismatch) |
| **GPE flash/image version** | `--version` in `ti/deploy_validate.sh` (date-derived) | — (programmer only) | CC35 vendor-RoT anti-rollback (unit rejects `<=` the programmed value) |

**App SemVer → `fw_version` marker.** The runtime u16 is *derived* from
`firmware-version.txt`, never hand-typed, so it cannot drift. Both build
paths parse the SemVer and pass the packed value in:
`CMakeLists.txt` (`target_compile_definitions`) and `ti/build_ti.sh`
(`-DCC3501E_BRIDGE_FW_VERSION_U16`). Pre-1.0 packing is
`(MINOR << 8) | PATCH`, so `0.2.0 → 0x0200`. `src/protocol.c` keeps an
`#ifndef` fallback equal to the current release for standalone compiles.

**GPE flash version** is *not* the app version. It is a monotonic
anti-rollback counter the vendor RoT enforces. `deploy_validate.sh`
derives it as `major.<yy>.<mmdd>.<hhmm>` (e.g. `1.26.0705.1432`) with
`major >= 1`, because the bench unit was poisoned to `0.9.0.7` by ad-hoc
bumps; `1.x` always beats it and every flash strictly increases.

## Selectable host-control transport

SPI is the default + always-available baseline; SDIO is opt-in and
mutually exclusive with a micro-SD card (the Alif has one SDIO
controller).  Both transports compile; `CC3501E_CONTROL_TRANSPORT`
selects which `main()` starts.  See README.md.

## Backends

- `stub` (`hal/cc3501e_hw_stub.c`): hardware-free; HW ops return
  `CC3501E_HW_ERR_NOTIMPL`.  The host test + CI compile smoke build this.
- `ti` (`hal/ti/`): the real bench backend, built with `ticlang` against
  TI's SimpleLink CC35xx SDK (FreeRTOS + LwIP + TI Drivers).  Implemented:
  - `cc3501e_hw_ti.c`: `GPIO_init`/`SPI_init`, lazy SimpleLink start,
    `cc3501e_hw_get_mac` via `sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET)`,
    deferred reset via CMSIS `NVIC_SystemReset()` after the ack is sent.
  - `transport_hw_ti_spi.c`: the four-phase hardware-SS0 SPI-slave
    transport above (`SPI_open(..., SPI_SLAVE, SPI_MODE_CALLBACK)`) with
    READY gating between phases.
  - `transport_hw_ti_sdio.c`: frame glue complete; SDIO-device register
    bring-up is the one SWRU626 §21 bench item (no public SDK SDIO-device
    driver) — off the v0.1 critical path.

## Next-rev hardening: host IRQ / async events

Hardware CS is already part of the current E1M-AEN bridge: Alif
`SPI1_SS0_C` frames each protocol phase.  The remaining board-level
hardening is a **host IRQ / DATA_READY** line for unsolicited events.
SPI and SDIO are mutually-exclusive control transports, so a future SPI
mode can reuse an SDIO-capable CC3501E pad when SDIO is not active:

| CC3501E pin | SDIO mode | SPI mode (future) |
|-------------|-----------|-------------------|
| `GPIO3`     | SDIO.CLK  | **HOST_IRQ** -> Alif `P7_0` (E1M `IO0`) |

The Alif is SPI master, so the CC3501E can never initiate, yet the
protocol defines async events (`EVT_WIFI_*`, `EVT_BLE_*`,
`EVT_GPIO_INTERRUPT`) with 5-10 ms latency budgets
(docs/cc3501e-bridge.md).  Polling cannot meet those budgets without
hammering the bus; a host-IRQ line is the standard SPI-coprocessor
solution and lets the host sleep until an event is ready.

The IRQ pad (`GPIO3` -> `P7_0`) consumes E1M `IO0` **only in SPI mode**;
in SDIO mode that pin is the SDIO clock and `IO0` is unaffected (this is
a per-transport pinmux, configured at build time alongside
`CC3501E_CONTROL_TRANSPORT`).  When the IRQ line lands, the firmware
raises it when an async event is ready and the host drains that event
with the existing four-phase request path.  Boot-safe: active-high with
an Alif pull-down; firmware drives it low early and the Alif arms the
interrupt only after the boot budget.

## Bench bring-up open items (AEN801)

The firmware is identical across all AEN SKUs (the CC3501E + its
inter-chip wiring are AEN-family-wide); these are board/SDK anchors, not
firmware redesigns:

1. **SysConfig anchor.** The `ti` build needs a board file defining
   `CONFIG_SPI_0` (the inter-chip SPI on CC3501E GPIO_27/28/29 plus the
   CC35-side CSN resource paired with Alif hardware SS0).
2. **Host IRQ / async events.** Add the board line and host event-drain
   path when the async-event work starts; command/reply traffic already
   uses hardware SS0 + READY.
3. **SDIO-device.** Implement the §21 register bring-up in
   `transport_hw_ti_sdio.c` if/when SDIO is needed (SPI is the default).
4. **Flashing.** `flash.py` / the `cc3501e_usb_bootloader` backend wait
   on TI's `cc3501e-flasher` CLI; until it ships, flash via SWD/J-Link.
