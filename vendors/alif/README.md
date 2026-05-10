# vendors/alif

Vendor wrapper for the **Alif Ensemble** family — backs the
**E1M-AEN** SoM family.

## SKUs covered

The E1M-AEN family ships six SKUs sharing one E1M routing and one
vendor HAL.  They differ only in Ensemble variant (which CPU/NPU
topology is bonded out) and MRAM size; this wrapper covers all six
without per-SKU branches.

| SKU            | Ensemble variant | Application CPU         | Real-time CPU                      | NPU (Ethos-U55)                |
|----------------|------------------|-------------------------|------------------------------------|--------------------------------|
| `E1M-AEN301`   | E3               | —                       | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 1× 204 GOPS       |
| `E1M-AEN401`   | E4               | —                       | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 1× 204 GOPS       |
| `E1M-AEN501`   | E5               | 1× Cortex-A32 @ 800 MHz | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 1× 204 GOPS       |
| `E1M-AEN601`   | E6               | 1× Cortex-A32 @ 800 MHz | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 2× 204 GOPS       |
| `E1M-AEN701`   | E7               | 2× Cortex-A32 @ 800 MHz | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 1× 204 GOPS       |
| `E1M-AEN801`   | E8               | 2× Cortex-A32 @ 800 MHz | 1× M55 @ 160 MHz + 1× M55 @ 400 MHz | 1× 46 GOPS + 2× 204 GOPS       |

Authoritative per-SKU detail (Alif part numbers, ISP / JPEG-encoder
options) lives in
[`e1m-spec` Annex A.1](https://github.com/alplabai/e1m-spec/blob/main/STANDARD.md#a1-e1m-aen-family-six-skus-shared-e1m-routing).

## Routing caveats

E1M-AEN exposes a **strict subset** of the E1M pinout (per
[`e1m-spec` §A.1](https://github.com/alplabai/e1m-spec/blob/main/STANDARD.md#a1-e1m-aen-family-six-skus-shared-e1m-routing)):

- Only `ETH0_*` is routed (Ensemble has one MAC).  `ETH1_*` is NC.
- Only `CSI0_*` is routed (one camera, 2 lanes).
- Only `DSI0_*` is routed (one display, 2 lanes).
- PCIe is not routed.
- Only USB 2.0 is exposed (`USB2_*`).
- Boot-strap pads `BOOT0`–`BOOT3` are NC; the AEN family does not
  support strap-driven boot-mode selection.
- `MODULE_STBY` enters real-time-clock standby only.

The vendor wrapper enforces these caveats — peripheral-open calls for
non-routed instances return `ALP_ERR_NOSUPPORT`.

## On-module support silicon (informative)

| Role | Part |
|---|---|
| Wi-Fi 6 + BLE 5.4 combo | TI `CC3501E` |
| Ethernet PHY (100 Mbps) | TI `DP83825I` |
| CAN transceiver | TI `TCAN1044AVDRBRQ1` |

## HAL pinning

The ALP SDK does not vendor Alif's HAL source — we link against
whatever revision the surrounding Zephyr tree pulls via
`modules/hal/alif`, or against a `cpackget`-installed
`AlifSemiconductor::Ensemble` CMSIS pack for plain-CMake / bare-metal
builds.

v0.1 targets the **latest stable Alif HAL revision supported by mainline
Zephyr**.  See `west.yml` and `docs/os-support-matrix.md` for the
exact pin once CI lands.

Implementation files (`i2c.c`, `spi.c`, `gpio.c`, `uart.c`,
`display.c`) will appear here when peripheral wrappers are signed off.
