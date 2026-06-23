# CC3501E Wi-Fi/BLE bridge architecture

The E1M-AEN family carries a separate **TI CC3501E** Wi-Fi 6 +
BLE 5.4 combo MCU on the SoM, distinct from the Alif Ensemble
main SoC.  This document explains how the two halves talk and
what lives where.

## Why two MCUs

The Alif silicon doesn't ship integrated Wi-Fi/BLE radios for
the AEN family -- the CC3501E is the wireless brain.  It also
proxies a handful of E1M GPIO pads (`IO11`, `IO13`,
`IO15`..`IO21`) and drives the camera-enable LDOs (CAM_EN_LDO0/1)
because TI's pad allocation hits those pins.  Net: any E1M pad
that terminates on the CC3501E (see
[`metadata/e1m_modules/aen/from-cc3501e.tsv`](../metadata/e1m_modules/aen/from-cc3501e.tsv))
must be reached via the CC3501E firmware over the inter-chip
SPI bus (Alif **SPI1** master ↔ CC3501E **SPI0** slave).

## Inter-chip wiring

Per [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv):

- **SPI** (Alif **SPI1** ↔ CC3501E **SPI0**): Alif `P14_4/5/6` ↔
  CC3501E `GPIO_27/28/29`.  Alif is master, CC3501E is slave.  Carries
  the host-control protocol.  (Note: the bus net is named `SPI1_*` after
  the Alif master; the CC3501E endpoint is its own SPI0 peripheral.)
- **SDIO**: shared with the SD-card slot via board-level
  resistors.  Pick one or the other -- not both at the same time.
- **WIFI_EN** (Alif `P15_5`): Alif drives the CC3501E
  power-enable line.
- **E_WIFI.NRST** (Alif `P15_1_FLEX`): Alif resets the CC3501E.
- **CAM_EN_LDO0 / LDO1** (CC3501E `GPIO_0` / `GPIO_1`): CC3501E
  enables the camera modules.

## Selectable host-control transport

The host-control link is **customer-selectable**, because different
customers trade pin count against Wi-Fi throughput differently:

| Transport | Role | CC3501E pins | Availability |
|-----------|------|--------------|--------------|
| **SPI0 slave** (CC35; Alif master = SPI1) | **DEFAULT** + always-available baseline/fallback | `GPIO_27/28/29` | Always |
| **SDIO slave** | OPTIONAL, higher throughput for Wi-Fi data | `GPIO_3/4/5/6/10/11` | Only when the board dedicates the Alif SDIO to the CC3501E |

The catch: the Alif Ensemble has a **single SDIO controller**, shared at
board level (mux resistors) with the micro-SD slot.  So SDIO is
available to the CC3501E **only on boards without an SD card** — when an
SD card is populated, SDIO is blocked and the link **must use SPI**.
SPI is therefore always wired and always the fallback.

The choice is a per-board integration decision: in a studio build it
comes from the customer `board.yaml`; for a hand-built firmware it's the
`CC3501E_CONTROL_TRANSPORT` CMake option (default `spi`).  Either way the
firmware brings up exactly one control transport, and both feed the same
command dispatcher — see
[`firmware/cc3501e/`](../firmware/cc3501e/) (`transport_spi.c` /
`transport_sdio.c`).

### Current rev: 3-wire SPI (no CS, no IRQ)

The current E1M-AEN board rev wires the inter-chip SPI as **3 wires
only — SCLK/MOSI/MISO**.  With no chip-select edge to frame
transactions, the host↔firmware exchange runs as four deterministic
fixed-count transfers in lockstep (request header → request payload →
reply header → reply payload), each side taking the next length from a
header it already exchanged; the host adds a short settle gap before
reading the reply.  This requires the CC3501E SS pad to be tied asserted
on the SoM and the SPI slave to complete on clock-count (SWRU626 §18).

The **coming board rev (AEN r2) adds two lines — CS + host-IRQ —
without spending new CC3501E pins**, by reusing the SDIO pins: SPI and
SDIO are mutually-exclusive control transports, so when SPI is active the
CC3501E's `GPIO3/4/5/6/10/11` are free for SPI-mode extras.  The planned
mapping is **`GPIO3` = HOST_IRQ → Alif `P7_0` (E1M `IO0`)** and a spare
SDIO pin for CS (in SDIO mode those are the SDIO bus, so `IO0` is
unaffected — it's a per-transport pinmux).  The IRQ line is the important
one: the Alif is SPI master, so the CC3501E can never initiate, yet the
protocol defines async events (`EVT_WIFI_*`, `EVT_BLE_*`,
`EVT_GPIO_INTERRUPT`) with the 5–10 ms latency budgets above; a host-IRQ
line is the standard SPI-coprocessor way to meet them without polling the
bus, and it also removes the reply settle gap.  The current rev (r1) is
unaffected.  See `firmware/cc3501e/DESIGN.md` "Next-rev hardening".

#### Bench-validated: why r1 is one-operation-at-a-time (2026-06-23)

The CS-less lockstep transport stays byte-aligned only while the radio is
**quiet between short operations**.  A radio op that keeps the NWP busy
**long enough that the SPI slave cannot be serviced** loses lockstep, and
without a CS edge to re-frame, the link does **not** recover on its own.
Measured on silicon:

| Operation | Radio-busy window | r1 bridge |
|---|---|---|
| `wifi scan` | ~3 s | survives (the `worker_run_pending` post-op `bridge_transport_spi_hw_reinit` re-syncs) |
| `ble scan` / `ble enable` | scan/enable window | survives |
| `wifi connect` (STA associate) | **~15 s association** | **permanent desync** — `GET_VERSION` returned IO-error for >24 s after, no recovery without a power-cycle |
| connected STA, or Wi-Fi + BLE both active | continuous | radio never goes quiet → same desync |

This is the SAME root cause behind the Wi-Fi-scan + BLE coexistence wall
(see `docs/cc3501e-production.md`): the limiter is the **transport**, not
SoftGemini and not the firmware logic.  It is exactly what the r2 **CS +
host-IRQ** lines fix — a CS edge re-frames every transaction so a busy
radio can no longer desync the link, and the host-IRQ removes the polling
that the lockstep depends on.  Until r2 (or an **SDIO** framed transport on
a board that dedicates the Alif SDIO to the CC3501E), the supported model
is **one radio operation at a time**: `wifi scan`, or `ble enable`+`ble
scan`, each from a clean state.  `wifi connect` is wired end-to-end
(host → bridge → `Wlan_Connect`, worker-routed off the SPI ISR) and the L2
association completes, but it desyncs the r1 link, so it is gated on r2.

## Boot model

The CC3501E has **no host-strap boot pin**.  Boot-mode selection
is internal to the chip -- driven by the *reset cause* + on-flash
state, not by an externally-strappable signal.  This was verified
against the datasheet (`cc3501e.pdf` pinout) and the user guide
(`swru626.pdf` Section 7.1.5 "Boot Sequence" + Section 7.2 "Reset").
The implication for the SDK: there is nothing additional for the
Alif side to wire beyond the two control lines we already have.

### What the host actually drives

| Signal       | Alif pad         | CC3501E pin    | Role                                  |
|--------------|------------------|----------------|---------------------------------------|
| `WIFI_EN`    | `P15_5`          | (power enable) | Gates the CC3501E supply.             |
| `E_WIFI.NRST`| `P15_1_FLEX`     | pin 49 `nRESET`| The only host-driven reset/boot signal. |
| `SPI1`       | `P14_4/5/6`      | `GPIO_27/28/29`| Host-control protocol post-boot.      |

That is the full host-visible boot surface.  No SOP0/SOP1/SOP2
straps (the CC3120/CC3220 generation had them; the CC3501E
dropped them), no boot-mode GPIO, no host-readable boot-config
register the SDK needs to touch.

### How the chip decides what to do at reset

Per UG Table 7-4, the ROM's first-stage bootloader keys off the
*reset cause source*:

| Reset cause                       | What the ROM does next                      |
|-----------------------------------|---------------------------------------------|
| Reset pin OR POR                  | Cold-boot, full Chain-of-Trust auth         |
| RVML / RVMH (rail voltage monitor, ~1.3 V) | Cold-boot                          |
| Brown-out (VDDMAIN < 1.71 V trip, SWRU626 §7.1.4) | Cold-boot                   |
| M33 WDT                           | Warm-boot (watchdog-recovery flow)          |
| Self-reset by M33                 | Warm-boot                                   |
| Debug-subsystem reset             | Boot into debug-aware path                  |

UG sections 34681..34901 describe four sub-flows the ROM can
take based on this + on-flash state:

1. **Device activation** -- normal boot, two-stage Chain-of-Trust
   off the application image in xSPI flash.
2. **Initial programming** -- factory first-flash flow, multi-stage.
3. **Reprogramming** -- field update flow.
4. **Wireless connectivity test tool** -- TI's manufacturing
   test mode.

All four are selected by reset-cause + on-flash state +
SimpleLink host commands -- never by an external strap.

### GPIO37 ("sensed at boot")

Datasheet footnote (3) on pin 52 says only:

> *"GPIO37 and Logger are sensed by the device during boot,
>  contact TI for more information."*

That is the entire public documentation.  Treat GPIO37 as
TI-internal -- do **not** reserve it for SDK use and do not
expect to drive it for boot-mode control.  The on-module
schematic should leave it tied per TI's default (PD, Hi-Z) or
to whatever TI recommends directly.

### Boot timing budget (UG Table 7-3)

| Phase | Name                                         | Typical  |
|-------|----------------------------------------------|----------|
| T1    | Supply settling                              | board    |
| T2    | Hardware init                                | ~20 ms   |
| T3    | Boot FW (1st-stage BL) init + auth           | ~380 ms  |
| T4    | Application image authentication             | ~500 ms  |
| **Total** | nRESET-release -> application running   | **~900 ms** |

The Alif-side bring-up code should hold off `ALP_CC3501E_CMD_PING`
attempts for at least 1 s after `WIFI_EN`/`nRESET` release.  The
inter-chip SPI handshake's first PING-reply also serves as the
"boot is complete, firmware is alive" signal.

### Where the firmware actually lives (4 MB xSPI flash)

The CC3501E exposes an internal xSPI bus (datasheet pinout:
`XSPI_D0..3`, `XSPI_CLK`, `XSPI_CS`).  The E1M-AEN module
populates a **4 MB external xSPI flash** on that bus carrying:

- BL2 (TI-signed 2nd-stage bootloader)
- Application image (SimpleLink + Wi-Fi stack + BLE stack +
  the embedded `firmware/cc3501e/` SPI/SDIO-slave parser)
- Filesystem region (TI SimpleLink file system for certs,
  service-pack, profile data)

**This flash is invisible to the Alif host.**  Reflash happens
exclusively via TI's `uniflash` / SimpleLink protocols on the
inter-chip SPI1 -- not via direct xSPI access from Alif.  If the
on-flash BL2 is ever corrupt, recovery goes through TI's tools,
not through any strap-pin recovery mode (there isn't one).

## Where the firmware lives (embedded in alp-sdk)

The firmware that runs on the CC3501E lives **in this repo** at
[`firmware/cc3501e/`](../firmware/cc3501e/) -- exactly as the
[`gd32-bridge`](../firmware/gd32-bridge/) firmware does.  Both are the
same class of artifact (an on-SoM helper-MCU bridge: custom binary
protocol, host driver in alp-sdk, prebuilt blob shipped in alp-sdk, its
own toolchain + version axis), so they are maintained the same way.  The
rationale -- and why the earlier "separate `alplabai/cc3501e-firmware`
repo" plan was dropped -- is [ADR 0015](adr/0015-cc3501e-firmware-embedded.md).

| Side | Lives in | Owns |
|------|----------|------|
| Host | `chips/cc3501e/` + `include/alp/protocol/cc3501e.h` | Alif-side SPI/SDIO client; `<alp/iot.h>` / `<alp/ble.h>` route-through. |
| Device | `firmware/cc3501e/` | Firmware on the CC3501E: SPI/SDIO-slave parser + TI SimpleLink Wi-Fi/AP + BLE 5.4 + GPIO proxy + camera-enable drivers. |

The firmware `#include`s the wire-protocol header **directly** (no
mirror), so a protocol change moves both sides + the wire-vector tests
in one commit.  The Alif-side client still refuses to talk to a firmware
whose `ALP_CC3501E_CMD_GET_VERSION` reply doesn't match the compile-time
`ALP_CC3501E_PROTOCOL_VERSION`.

## Firmware: pre-flashed by Alp; rebuild is optional

**The CC3501E ships pre-flashed by Alp** with the bridge firmware — for
normal use the customer flashes and configures nothing; the device boots
the bridge and the Alif-side `<alp/...>` calls work out of the box.  A
version-pinned prebuilt blob also lives at
`firmware/cc3501e/prebuilt/cc3501e-vX.Y.Z.bin` for field re-flash.

Rebuilding or customizing the firmware is **optional and open** — the
bridge firmware source is Alp's (public, like the GD32 bridge), in-tree
at [`firmware/cc3501e/`](../firmware/cc3501e/), built on TI's
**BSD-3-licensed** SimpleLink Wi-Fi SDK.  The in-tree firmware:

1. Vendors TI's **BSD-3-licensed** SimpleLink CC33xx SDK as an optional
   git submodule under `firmware/cc3501e/vendor/simplelink-cc33xx/` —
   only the bench `ti` build pulls it; not recursed by default.  Same
   shape as the GD32 GigaDevice library.
2. Builds with TI Code Composer Studio or the open `ticlang` toolchain
   (`firmware/cc3501e/toolchain/ticlang.cmake`).
3. `#include`s this repo's `include/alp/protocol/cc3501e.h` **directly**
   — no mirrored copy, so the two sides cannot drift.
4. Wires commands into TI's SimpleLink Wi-Fi APIs (`sl_*`) and the BLE
   host (CC3501E ships an Apache-licenced BLE 5.4 host) — v0.2+.
5. Drives `GPIO_0`, `GPIO_1` (camera enables) and the GPIO-proxy pads
   (`GPIO_2`, `GPIO_13..30` per `from-cc3501e.tsv`) — v0.4.
6. Tags releases `vX.Y.Z`; the signed binary lands at
   `firmware/cc3501e/prebuilt/cc3501e-vX.Y.Z.bin` so consumers can flash
   without rebuilding.
7. Hands off via `firmware/cc3501e/flash.py` on USB / debug probe.

See [`firmware/cc3501e/README.md`](../firmware/cc3501e/README.md) for the
build + tree layout and `firmware/cc3501e/DESIGN.md`
for the v0.1 scope + wire-reply contract.

## Versioning

Three independent version axes (same model as the GD32 bridge) — track
them separately:

| Axis | Where | Bumps when |
|------|-------|-----------|
| **Firmware release** | `firmware/cc3501e/firmware-version.txt` (semver) | each firmware release — names the tag + the `cc3501e-vX.Y.Z.bin` prebuilt blob; the device reports it as `fw_version` via `GET_VERSION` / `GET_DIAG_INFO` |
| **Wire protocol** | `ALP_CC3501E_PROTOCOL_VERSION` (`<alp/protocol/cc3501e.h>`) + `firmware/cc3501e/protocol-version.txt` | the wire format changes; the host refuses a mismatched version |
| **Build / signature** | the signed binary's `.sha256` in `prebuilt/` | every build — pins the exact image |

The firmware version moves on its own cadence — a release can ship
without a protocol bump, and vice-versa.

## Firmware-side GPIO behaviour contract

The EVK schematic walkthrough surfaced specific behaviours the
CC3501E firmware MUST implement on its proxied pads.  These are
contractual: the Alif-side `alp_gpio_*` calls expect these
modes to "just work" through `ALP_CC3501E_CMD_GPIO_CONFIGURE` +
`ALP_CC3501E_CMD_GPIO_WRITE` + `ALP_CC3501E_CMD_GPIO_SET_INTERRUPT`.

### Open-drain output

Required for M.2 E-key `W_DISABLE1` (CC3501E `GPIO_17` ↔ E1M
`IO17`, Wi-Fi disable) and `W_DISABLE2` (CC3501E `GPIO_16` ↔
E1M `IO16`, Bluetooth disable).  Per the M.2 spec these are
open-drain active-low: write 0 to assert, write 1 (or
Hi-Z / release) to deassert.  Firmware must:

- Accept `ALP_CC3501E_GPIO_DIR_OPEN_DRAIN` in the configure
  command's direction field.
- On write 0, drive the pad LOW.
- On write 1, release the pad to Hi-Z (do NOT drive it HIGH --
  the M.2 card has its own pull-up to its rail).
- On read, return the actual pad level (so the host can see
  whether another open-drain source on the bus is asserting).

### Edge-triggered interrupt forwarding

Three event sources need rising / falling / either-edge
interrupt forwarding back to Alif via the protocol's GPIO
event packets:

- **`M2E_UART_WAKE`** (CC3501E `GPIO_19` ↔ E1M `IO19`):
  falling edge (M.2 module asserts to wake host).
- **`M2E_SDIO_WAKE`** (CC3501E `GPIO_18` ↔ E1M `IO18`):
  falling edge, same semantics.
- **`BMI323_INT1`** (CC3501E `GPIO_14` ↔ E1M `IO15`):
  configurable edge (BMI323 INT1 can be either polarity via
  its `INT_CONF` register; the firmware just forwards whichever
  edge the Alif-side requested).

Latency budget: the wake signals should propagate to the Alif
within 10 ms of edge.  BMI323 INT1 should propagate within 5 ms
when the FIFO threshold is the interrupt source (so motion
events don't sit in a queue).

### Safe-default mux state

On boot, before the Alif-side has connected over SPI1, the
firmware should drive its proxied mux-control pins to states
that ISOLATE all the downstream buses:

- `SDIO_MUX_EN` (`GPIO_26`): HIGH (74LVC157 /E = 1 → Hi-Z).
- `SDIO_MUX_SEL` (`GPIO_30`): don't-care while /E is high.
- `I2S_MUX_SEL` (`GPIO_13`): don't-care (the /E pin is on the
  Alif side and defaults to disable).
- `USB2_MUX_SEL` (`GPIO_2`): default to 0 (USB-A connector
  routed; M.2 E-key USB isolated).

The Alif's bring-up code then sequences enables once it's ready.

## v0.x roadmap

| Step                                             | Where it lands                          |
|--------------------------------------------------|-----------------------------------------|
| Wire protocol v1 frozen                          | `include/alp/protocol/cc3501e.h` ✅ |
| Alif-side SPI client (`chips/cc3501e/`)          | ✅ landed in alp-sdk              |
| `<alp/iot.h>` / `<alp/ble.h>` route via CC3501E  | Follow-up: dispatcher branch in iot_zephyr.c / ble_zephyr.c |
| Firmware tree (embedded)                         | `firmware/cc3501e/` ✅ (per [ADR 0015](adr/0015-cc3501e-firmware-embedded.md)) |
| Bring-up firmware (PING + GET_VERSION + GET_MAC + RESET) | `firmware/cc3501e/` v0.1 ✅ (silicon-free + stub; bench build pending) |
| Wi-Fi station mode                               | `firmware/cc3501e/` v0.2            |
| BLE peripheral + advertise                       | `firmware/cc3501e/` v0.3            |
| GPIO proxy + camera-enable                       | `firmware/cc3501e/` v0.4            |
| Full feature parity with `<alp/iot.h>` /
  `<alp/ble.h>`                                    | `firmware/cc3501e/` v1.0            |

## See also

- Wire protocol header: [`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h)
- Inter-chip wiring data: [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv)
- E1M ↔ CC3501E pad routing: [`metadata/e1m_modules/aen/from-cc3501e.tsv`](../metadata/e1m_modules/aen/from-cc3501e.tsv)
- Repo-boundary acid test: [ADR 0005](adr/0005-alp-sdk-vs-alp-studio-boundary.md)
