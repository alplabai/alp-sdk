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
side: `chips/cc3501e/cc3501e_core.c` `cc3501e_request()` (matching four-phase
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
| **Wire protocol version** | `ALP_CC3501E_PROTOCOL_VERSION` in `<alp/protocol/cc3501e.h>` (currently `5`) | `GET_VERSION` (0x01) | host↔firmware wire compatibility (host refuses a mismatch — enforced by `cc3501e_reset()` in `chips/cc3501e/cc3501e_core.c`, which reads `GET_VERSION` once the cold boot completes and returns `ALP_ERR_VERSION` if the reply differs from the host's compile-time value; #1371) |
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

## Power policy: both halves run on the TASK, never in the dispatch ISR

`handle_power_policy` (0x62) is reached from the SPI-dispatch ISR, and **neither**
half of the policy may run there:

- the radio half calls `Wlan_Set()`, a blocking vendor call -- the same reason
  `handle_sock_recv()` cannot call `lwip_recv()` and the worker seam exists;
- the core half calls `Power_setPolicy()` / `Power_enablePolicy()`, which swap and
  re-arm the function pointer the idle loop runs, racing the idle loop that may be
  executing the very policy being replaced.

Running either inline did not merely fail: on silicon every preset returned `-4`
and the bridge itself went to `PING -> -5`. Both are therefore latched by the ISR
handler and drained by `cc3501e_hw_power_service()` from `cc3501e_hw_tick()`, core
first then radio.

**The wire consequence: a `RESP_OK` to `POWER_POLICY` means QUEUED, not APPLIED** --
the same semantic `OTA_BEGIN` turned out to have. The reply carries one data byte
reporting whether the *previous* radio apply was realised, surfaced to the host as
`cc3501e_power_policy()`'s `radio_ok_out`. Without it a host cannot distinguish an
accepted-and-applied policy from an accepted-and-silently-rejected one, which is
exactly how a broken radio apply went unnoticed on the bench.

`Wlan_Set()` is also rejected while the radio is down, so the latched policy is
re-applied after `Wlan_RoleUp(STA)` succeeds; otherwise a policy set before Wi-Fi
came up is dropped.

> **[#1691](https://github.com/alplabai/alp-sdk/issues/1691):** repeated BLE
> advertise/stop cycles can wedge the bridge. It is NOT power-related — it
> reproduces with no power policy applied. Diagnostics across the fault show the
> slave armed and idle in `PH_REQ_HEADER`, READY HIGH, `g_resync_count` and
> `g_arm_fail_count` both zero, and SPI transfers still completing, so no firmware
> self-heal can see it: `bridge_transport_spi_is_dead()` only reports a failed
> `SPI_open`, and the stall watchdog deliberately ignores `PH_REQ_HEADER`. Recovery
> is host-side via `cc3501e_recover()` (warm reset), which cleared every observed
> wedge. The `CC3501E_WEDGE_PROBE` build flag captures the state into `.TI.noinit`
> for further investigation.

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

## Async events: the attention edge (shipped), and a dedicated HOST_IRQ (future)

The Alif is SPI master, so the CC3501E can never initiate a transfer, yet the
protocol defines async events (`EVT_WIFI_*`, `EVT_BLE_*`,
`EVT_GPIO_INTERRUPT`) with 5-10 ms latency budgets
(docs/cc3501e-bridge.md).  Polling cannot meet those budgets without
hammering the bus.

### What shipped (#130, alp-sdk#1721 -- 2026-08-27)

Rather than wait for a board rev, the slave->master attention path was
**multiplexed onto the existing READY wire** (CC35 `GPIO17` -> Alif `P2_6`,
the rev-1 bodge line).  The firmware pulses that line when it queues an
event (`cc3501e_bridge_attn_pulse()`, called from `event_ring_push()`); the
host arms an edge interrupt with `cc3501e_attn_arm()` and drains on the
edge.  Validated on silicon: the host application saw 135 of 135 firmware
pushes across a 40 s armed window with the console timer poll set to 60 s,
so no timer delivery was possible.

Three properties of the shared wire are load-bearing, not incidental:

- **Sticky arm intent.**  The ISR masks the line and the request lock
  re-arms it, but ONLY inside an explicit `cc3501e_attn_arm(ctx, true)`
  window.  Unconditional re-arming in `lock_release()` hangs the device
  after `WIFI_SCAN`.
- **Empty-drain backoff (50 ms).**  The wire also carries READY flow
  control, so a transaction end is indistinguishable from an attention
  pulse and a drain's own trailing READY rise re-triggers the ISR.  A drain
  that delivered zero events backs off; one that delivered events re-arms
  immediately.  Without it: 11888 ISRs for 86 events.  With it: 220, with
  nothing lost.
- **A full ring must still pulse.**  `event_ring_push()` raises attention on
  a DROPPED push too.  Pulsing only on success is unrecoverable on an idle
  host: 16 boot-time Wi-Fi events fill `CC3501E_EVENT_RING_SLOTS`, after
  which no pulse means no drain means still full.

### Two limits to know before relying on it

- **The pulse is a build-time opt-in.**  `build_ti.ps1 -AttnPulse` sets
  `-DCC3501E_ATTN_PULSE=1`; it is **default OFF** because the wire is a
  rev-1 bodge absent on the stock EVK, and the pulse is an extra transition
  on the line every host uses for flow control.  `package_cc3501e_prod.ps1`
  does not pass it.  A build without the flag links a no-op stub, so a host
  that arms attention never gets an edge and falls back to its timer poll.
- **Only Wi-Fi events have a producer.**  `EVT_WIFI_CONNECTED` and
  `EVT_WIFI_DISCONNECTED` are pushed into the ring; `EVT_GPIO_INTERRUPT`
  and the BLE events are not pushed by anything today.  The transport is
  not the blocker for those -- an `event_ring_push()` call at the producer
  is.

### Still future: a dedicated HOST_IRQ line

Sharing the wire with flow control is what forces the empty-drain backoff
above.  A future board rev can give attention its own pad.  SPI and SDIO
are mutually-exclusive control transports, so a future SPI mode can reuse
an SDIO-capable CC3501E pad when SDIO is not active:

| CC3501E pin | SDIO mode | SPI mode (future) |
|-------------|-----------|-------------------|
| `GPIO3`     | SDIO.CLK  | **HOST_IRQ** -> Alif `P7_0` (E1M `IO0`) |

That pad consumes E1M `IO0` **only in SPI mode**; in SDIO mode the pin is
the SDIO clock and `IO0` is unaffected (a per-transport pinmux, configured
at build time alongside `CC3501E_CONTROL_TRANSPORT`).  Boot-safe:
active-high with an Alif pull-down; firmware drives it low early and the
Alif arms the interrupt only after the boot budget.

## Bench bring-up open items (AEN801)

The firmware is identical across all AEN SKUs (the CC3501E + its
inter-chip wiring are AEN-family-wide); these are board/SDK anchors, not
firmware redesigns:

1. **SysConfig anchor.** The `ti` build needs a board file defining
   `CONFIG_SPI_0` (the inter-chip SPI on CC3501E GPIO_27/28/29 plus the
   CC35-side CSN resource paired with Alif hardware SS0).
2. **Async-event producers.** The attention transport shipped (#130,
   alp-sdk#1721) on the shared READY wire; what is missing is producers.
   Only `EVT_WIFI_CONNECTED` / `EVT_WIFI_DISCONNECTED` reach
   `event_ring_push()`.  `EVT_GPIO_INTERRUPT` and the BLE events need a
   push at the source, not new hardware.  A dedicated HOST_IRQ pad is a
   separate, later board-rev item.
3. **SDIO-device.** Implement the §21 register bring-up in
   `transport_hw_ti_sdio.c` if/when SDIO is needed (SPI is the default).
4. **Flashing.** The CC3501E is Alp-OTA-updated (`update_channel:
   alp_ota_spi_otp`, `flash_policy: recovery_only`); a customer flash
   is permitted only to recover a bricked device, with Alp Lab-supplied
   binaries.  Bench units are warm-programmed via SWD/J-Link (see
   `docs/cc3501e-production.md`).
   The retired public `cc3501e_usb_bootloader` backend and
   `flash.py` release/bench helper now live in `alp-sdk-internal`
   as Alp-internal OTA-build tooling.
