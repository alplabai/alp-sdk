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
SPI1 bus.

## Inter-chip wiring

Per [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv):

- **SPI1**: Alif `P14_4/5/6` ↔ CC3501E `GPIO_27/28/29`.  Alif is
  master, CC3501E is slave.  Carries the host-control protocol.
- **SDIO**: shared with the SD-card slot via board-level
  resistors.  Pick one or the other -- not both at the same time.
- **WIFI_EN** (Alif `P15_5`): Alif drives the CC3501E
  power-enable line.
- **E_WIFI.NRST** (Alif `P15_1_FLEX`): Alif resets the CC3501E.
- **CAM_EN_LDO0 / LDO1** (CC3501E `GPIO_0` / `GPIO_1`): CC3501E
  enables the camera modules.

## Two-repo split

Per [ADR 0005](adr/0005-alp-sdk-vs-alp-studio-boundary.md)'s
dual-use acid test, the firmware that runs on the CC3501E lives
in its own repository -- it ships TI's SimpleLink CC33xx SDK,
licences differently, releases on its own cadence, and most
SDK consumers never rebuild it.

| Repo                              | Owns                                                                        |
|-----------------------------------|-----------------------------------------------------------------------------|
| `alplabai/alp-sdk` (this repo)    | Wire-protocol header (`include/alp/protocol/cc3501e.h`); Alif-side SPI client (`chips/cc3501e/`); `<alp/iot.h>` / `<alp/ble.h>` route-through. |
| `alplabai/cc3501e-firmware` (TBD) | Firmware that runs on the CC3501E MCU: TI SimpleLink + Wi-Fi station/AP + BLE 5.4 stack + SPI-slave protocol parser + GPIO proxy + camera-enable drivers. |

The two are linked by a stable wire protocol (defined in this
repo, version-tagged via `ALP_CC3501E_PROTOCOL_VERSION`).
Firmware releases pin to a protocol version; the Alif-side
client refuses to talk to a firmware whose
`ALP_CC3501E_CMD_GET_VERSION` reply doesn't match the
compile-time expectation.

## Firmware bootstrap (for `alplabai/cc3501e-firmware`)

When that repo is created, it should:

1. Vendor TI's SimpleLink CC33xx SDK as a git submodule.
2. Build with TI Code Composer Studio or the open `ticlang`
   toolchain.
3. Implement the SPI-slave parser against this repo's
   `include/alp/protocol/cc3501e.h` -- a copy of that header
   sits in the firmware's `include/alp/` for source-of-truth
   consistency.
4. Wire commands into TI's SimpleLink Wi-Fi APIs (`sl_*`) and
   the BLE host (CC3501E ships an Apache-licenced BLE 5.4 host).
5. Drive `GPIO_0`, `GPIO_1` (camera enables) and the GPIO-proxy
   pads (`GPIO_2`, `GPIO_13..30` per `from-cc3501e.tsv`).
6. Tag releases `vX.Y.Z`; the binary blob lands at
   `firmware/cc3501e/prebuilt/cc3501e-vX.Y.Z.bin` here in alp-sdk
   so consumers can flash without cloning the firmware repo.
7. Hand-off via `update_cc3501e.py` (or the existing TI flash
   utility if one is available) on USB or via a JTAG probe
   wired to the CC3501E's debug pads.

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
| Wire protocol v1 frozen                          | `include/alp/protocol/cc3501e.h` (this commit) |
| Alif-side SPI client (`chips/cc3501e/`)          | Follow-up commit in alp-sdk              |
| `<alp/iot.h>` / `<alp/ble.h>` route via CC3501E  | Follow-up: dispatcher branch in iot_zephyr.c / ble_zephyr.c |
| Firmware repo bootstrap                          | `alplabai/cc3501e-firmware` (created separately) |
| Bring-up firmware (PING + version)               | Firmware repo v0.1                       |
| Wi-Fi station mode                               | Firmware repo v0.2                       |
| BLE peripheral + advertise                       | Firmware repo v0.3                       |
| GPIO proxy + camera-enable                       | Firmware repo v0.4                       |
| Full feature parity with `<alp/iot.h>` /
  `<alp/ble.h>`                                    | Firmware repo v1.0                       |

## See also

- Wire protocol header: [`include/alp/protocol/cc3501e.h`](../include/alp/protocol/cc3501e.h)
- Inter-chip wiring data: [`metadata/e1m_modules/aen/inter-chip.tsv`](../metadata/e1m_modules/aen/inter-chip.tsv)
- E1M ↔ CC3501E pad routing: [`metadata/e1m_modules/aen/from-cc3501e.tsv`](../metadata/e1m_modules/aen/from-cc3501e.tsv)
- Repo-boundary acid test: [ADR 0005](adr/0005-alp-sdk-vs-alp-studio-boundary.md)
