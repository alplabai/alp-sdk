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

## Wire framing — 3-wire deterministic lockstep (this HW rev)

Both sides build the same frame: a 4-byte LE header
`[cmd | flags | payload_len(LE16)]` + payload.  The reply echoes the
request `cmd`, uses `flags = 0` (solicited; async events in v0.2+ set
`ALP_CC3501E_FLAG_ASYNC_EVENT`), and its payload is `[status][data...]`
— the response status (`ALP_CC3501E_RESP_*`) is the first payload byte,
per the header.  Framing + dispatch is centralised in
`protocol_build_reply()` so SPI and SDIO are byte-identical.

The **current E1M-AEN rev wires only SCLK/MOSI/MISO** — no CS, no host
IRQ (see "Next-rev hardening").  With no CS edge to delimit
transactions, a request/reply is clocked as **four fixed-count transfers
in deterministic lockstep**, each side deriving the next length from a
header it already exchanged:

| # | master clocks | direction | length |
|---|---------------|-----------|--------|
| 1 | request header | MOSI | 4 |
| 2 | request payload | MOSI | `payload_len` (from #1) |
| 3 | reply header | MISO | 4 |
| 4 | reply payload | MISO | reply `payload_len` (from #3) = status + data |

The host adds a short settle gap before step 3 so the slave can dispatch
and arm the reply (v0.1 META dispatch is instant).  No CS means the
CC3501E SS pad must be tied to its asserted level on the SoM and
`SPI_transfer()` must complete on clock-count alone — **confirm against
SWRU626 §18**.  Firmware side: `hal/ti/transport_hw_ti_spi.c` (a
`SPI_SLAVE` + `SPI_MODE_CALLBACK` state machine that replays the captured
frame through the silicon-free byte seams).  Host side:
`chips/cc3501e/cc3501e.c` `cc3501e_request()` (matching 4-transfer
sequence + `resp_to_status()` on `payload[0]`).  Host + firmware are
reconciled to each other and to the header; both are `[UNTESTED]` until
the AEN801 bench.

GET_VERSION returns the *protocol* version (the host's compat gate),
not the firmware *release* version — the diag-struct comment that
implied otherwise was a header doc slip (now fixed); the release version
is surfaced via `GET_DIAG_INFO.fw_version` in v2.

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
  - `transport_hw_ti_spi.c`: the 4-transfer 3-wire SPI-slave lockstep
    above (`SPI_open(..., SPI_SLAVE, SPI_MODE_CALLBACK)`); no CS/IRQ.
  - `transport_hw_ti_sdio.c`: frame glue complete; SDIO-device register
    bring-up is the one SWRU626 §21 bench item (no public SDK SDIO-device
    driver) — off the v0.1 critical path.

## Next-rev hardening: CS + host IRQ, reusing the SDIO pins

The 3-wire link works for v0.1 bring-up but is intentionally minimal.
The next board rev (AEN **r2**) adds two CC3501E↔Alif lines — a **CS**
and a **host IRQ / DATA_READY** — without spending new CC3501E pins, by
**reusing the SDIO pins**.  SPI and SDIO are mutually-exclusive control
transports, so the CC3501E's `GPIO3/4/5/6/10/11` are free for SPI-mode
extras whenever SDIO isn't the active transport:

| CC3501E pin | SDIO mode | SPI mode (r2) |
|-------------|-----------|---------------|
| `GPIO3`     | SDIO.CLK  | **HOST_IRQ** → Alif `P7_0` (E1M `IO0`) |
| `GPIO4`/... | SDIO.CMD/D* | **CS** (one spare SDIO pin — TBD) |

- **CS** restores hardware transaction framing + a desync-recovery edge
  (the 3-wire lockstep can't recover if a stray clock desyncs it).
- **host IRQ** is the bigger win: the Alif is SPI master, so the CC3501E
  can never initiate, yet the protocol defines async events
  (`EVT_WIFI_*`, `EVT_BLE_*`, `EVT_GPIO_INTERRUPT`) with 5–10 ms latency
  budgets (docs/cc3501e-bridge.md).  Polling can't meet those without
  hammering the bus; a host-IRQ line is the standard SPI-coprocessor
  solution and also removes the reply settle gap.

The IRQ pad (`GPIO3`→`P7_0`) consumes E1M `IO0` **only in SPI mode**;
in SDIO mode that pin is the SDIO clock and `IO0` is unaffected (this is
a per-transport pinmux, configured at build time alongside
`CC3501E_CONTROL_TRANSPORT`).  When r2 lands, the firmware raises IRQ
when a reply/event is ready and the host waits on it instead of the
settle gap — the `cc3501e_request()` 4-transfer shape is unchanged, just
gated by IRQ instead of a delay.  Boot-safe: active-high with an Alif
pull-down; firmware drives it low early and the Alif arms the interrupt
only after the boot budget.

## Bench bring-up open items (AEN801)

The firmware is identical across all AEN SKUs (the CC3501E + its
inter-chip wiring are AEN-family-wide); these are board/SDK anchors, not
firmware redesigns:

1. **SysConfig anchor.** The `ti` build needs a board file defining
   `CONFIG_SPI_0` (the inter-chip SPI on CC3501E GPIO_27/28/29).
2. **§18 confirm.** Verify the SPI slave runs CS-less (SS tied asserted)
   and `SPI_transfer()` completes on clock-count — the 3-wire assumption.
3. **SDIO-device.** Implement the §21 register bring-up in
   `transport_hw_ti_sdio.c` if/when SDIO is needed (SPI is the default).
4. **Flashing.** `flash.py` / the `cc3501e_usb_bootloader` backend wait
   on TI's `cc3501e-flasher` CLI; until it ships, flash via SWD/J-Link.
