# cc3501e-bridge firmware — design notes

Scope, wire-reply contract, and the cross-side reconciliation items
surfaced while authoring v0.1.  The authoritative wire contract is
[`include/alp/protocol/cc3501e.h`](../../include/alp/protocol/cc3501e.h);
this file records the firmware-side decisions and the gaps that need a
matching host-side change.

## v0.1 scope ("bring-up")

The META command group only, enough to prove the link is alive and
version-compatible:

| Opcode | Behaviour |
|--------|-----------|
| `PING` (0x00) | empty reply payload → `RESP_OK`; the liveness signal |
| `GET_VERSION` (0x01) | reply data = `ALP_CC3501E_PROTOCOL_VERSION` (u16 LE) |
| `GET_MAC` (0x03) | reply data = 6-byte factory MAC (HAL); stub → `RESP_ERR_NOT_READY` |
| `RESET` (0x02) | ack `RESP_OK`, then HAL reboots after the ack is drained |

Every other opcode (Wi-Fi, BLE, GPIO proxy, camera, power, diagnostics)
returns `ALP_CC3501E_RESP_ERR_INVALID`, per the protocol header's
contract that v1 firmware rejects opcodes it does not implement.  Those
groups land in v0.2+ and route to TI's SimpleLink Wi-Fi / BLE APIs
through the `hal/ti/` backend.

## Wire-reply framing (firmware side)

The reply frame mirrors the request: a 4-byte LE header
`[cmd | flags | payload_len(LE16)]` + payload.  The firmware:

- echoes the request `cmd` in reply byte 0 (lets a corrected host
  correlate reply↔request),
- sets `flags = 0` (a solicited reply; async-event frames in v0.2+ set
  `ALP_CC3501E_FLAG_ASYNC_EVENT`),
- puts the response status (`ALP_CC3501E_RESP_*`) as the **first payload
  byte**, per the header's stated convention, followed by any data.

Framing + dispatch is centralised in `protocol_build_reply()` so the
SPI and SDIO transports are byte-identical on the wire.

## Cross-side reconciliation items (host driver)

Authoring the firmware surfaced two places where the **executable**
host driver (`chips/cc3501e/cc3501e.c`, marked `[UNTESTED]`) and/or the
header prose disagree.  Nothing is on silicon yet and there are no
active customers, so the firmware is reconciled to the **authoritative
header**, and the host driver's v0.x bring-up rework must catch up.
These are the #1/#2 follow-ups before HiL:

1. **Reply-read handshake.**  `cc3501e_request()` currently reads the
   reply inside the *same* full-duplex `alp_spi_transceive()` as the
   request.  A real SPI slave cannot do that -- it only knows the reply
   *after* the request is fully clocked.  The firmware stages the reply
   for a **separate** read transaction and (per the driver's own TODO)
   needs a firmware **READY GPIO** so the host knows when to read.  The
   host driver must move to the two-transaction + READY model.

2. **Status-byte parsing + GET_VERSION.**  The header says "response
   status codes [are] carried in the first byte of every response
   payload", and this firmware follows that.  But `cc3501e_request()`
   returns the raw payload without inspecting a status byte, and
   `cc3501e_get_version()` treats the whole payload as the version.  The
   host driver must (a) read `payload[0]` as the status and (b) read the
   version from `payload[1..2]`.  Relatedly, the diag-struct comment in
   the header that says GET_VERSION returns the firmware *release*
   version is a documentation slip -- GET_VERSION returns the *protocol*
   version (the host's compat gate compares it to
   `ALP_CC3501E_PROTOCOL_VERSION`); the release version is reported via
   `GET_DIAG_INFO.fw_version` in v2.

## Selectable host-control transport

SPI is the default + always-available baseline; SDIO is opt-in and
mutually exclusive with a micro-SD card (the Alif has a single SDIO
controller).  Both transports compile; `CC3501E_CONTROL_TRANSPORT`
selects which `main()` starts.  See README.md "Selectable host-control
transport".

## Backends

- `stub` (`hal/cc3501e_hw_stub.c`): hardware-free; HW ops return
  `CC3501E_HW_ERR_NOTIMPL`.  The host test + CI compile smoke build this.
- `ti` (`hal/ti/`): the real bench backend, built with `ticlang` against
  TI's SimpleLink CC35xx SDK (FreeRTOS + LwIP + TI Drivers).  Implemented,
  not stubbed:
  - `cc3501e_hw_ti.c`: `cc3501e_hw_init` (TI Drivers `GPIO_init`/`SPI_init`),
    lazy SimpleLink start, `cc3501e_hw_get_mac` via
    `sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET)`, and a deferred reset via
    CMSIS `NVIC_SystemReset()` (after the ack is clocked out).
  - `transport_hw_ti_spi.c`: SPI-slave via `SPI_open(..., SPI_SLAVE,
    SPI_MODE_CALLBACK)`.  Because TI Drivers SPI is transfer-oriented, a
    frame is clocked as three fixed-count transfers — 4-byte header, then
    a payload of the declared length, then the staged reply — with a
    READY GPIO as the per-chunk flow-control edge.  The completed frame
    is replayed through the silicon-free byte seams, so framing/dispatch
    (and the host test) are identical to the stub path.
  - `transport_hw_ti_sdio.c`: frame glue complete; SDIO-device register
    bring-up is the one SWRU626 §21 bench item (no public SDK SDIO-device
    driver) — off the v0.1 critical path.

## TI backend bench bring-up (open items)

These are the things to settle when the hardware (first board: **E1M-AEN801**;
the firmware is identical across all AEN SKUs — the CC3501E + its inter-chip
wiring are AEN-family-wide) is on the bench.  None require firmware redesign;
they pin board-specific anchors and the host side.

1. **SysConfig anchors.** The `ti` build needs a SysConfig board file
   defining `CONFIG_SPI_0` (the inter-chip SPI on CC3501E GPIO_27/28/29)
   and `CONFIG_GPIO_CC3501E_HOST_READY` (the READY output to the Alif).
2. **Reply-timing handshake + READY line.** The SPI-slave reply rides a
   separate transaction gated by a CC3501E→Alif READY line.  `inter-chip.tsv`
   does not yet show a dedicated READY/IRQ pad (only `RTC_ALARM` on Alif
   `P15_0_FLEX`).  Decide: route a READY line (or repurpose one) vs. a
   poll-on-MISO scheme.  This determines the host-driver rework below.
3. **Host-driver reconciliation (#1 from above).** Once the handshake is
   fixed, rework `chips/cc3501e/cc3501e.c` `cc3501e_request()` to clock
   header → (READY) → payload → (READY) → reply and parse `payload[0]` as
   the status.  Held until the READY decision so it's not built on a guess.
4. **SDIO-device.** Implement the §21 register bring-up in
   `transport_hw_ti_sdio.c` if/when SDIO is needed (SPI is the default).
5. **Flashing.** `flash.py` / the `cc3501e_usb_bootloader` backend wait on
   TI's `cc3501e-flasher` CLI; until it ships, flash via SWD/J-Link.
