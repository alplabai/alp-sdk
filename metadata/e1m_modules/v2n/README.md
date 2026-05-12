# E1M-X V2N module pinout

Pin-to-function mapping for the E1M-X V2N family of SoMs
(`E1M-V2N101`, `E1M-V2N102` -- Renesas RZ/V2N-based modules
without the DEEPX DX-M1 NPU).

## Files

| File                              | Schema                                   |
|-----------------------------------|------------------------------------------|
| `renesas-peripheral-map.tsv`      | `peripheral \t renesas_pad`              |
| `renesas-peripheral-map.csv`      | `row, peripheral, renesas_pad`           |
| `gd32-io-mcu-map.tsv`             | `peripheral \t gd32_pad`                 |
| `gd32-io-mcu-map.csv`             | `row, peripheral, gd32_pad`              |
| `hw-revisions.yaml`               | Per-rev SDK-version compatibility window |

## Two MCUs on the module

The V2N module's E1M-edge peripherals split across two silicon
sources:

- **Renesas RZ/V2N** (the application SoC) -- owns I²C, SPI, UART,
  I²S, classic RGMII Ethernet, CAN, SD/eMMC, xSPI NOR, PDM, and the
  DRP-AI3 accelerator.  Renesas drives **none** of the eight E1M
  PWM channels directly; all PWMs are GD32-driven.
- **GigaDevice GD32G553MEY7TR** (companion IO MCU) -- owns **all
  eight E1M PWM channels**, the encoder-input bank, the dual
  ADC + DAC bank, the camera-LDO enables, the Murata Wi-Fi/BT
  module's REG_ON pins, and the OPTIGA reset.  Reached from the
  Renesas side via the **GD32 bridge** -- see
  [`../../../docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md).

The two MCUs share a board-management I²C bus (BRD_I2C) plus a
dedicated SPI link.

## V2N-M1 vs V2N base

`E1M-V2M101` / `E1M-V2M102` (the V2N-M1 family) reuses this base
map plus a small Renesas-side overlay for the DEEPX-specific
signals (`M1_RESET`, `PCIe.MUX_PD`, `PCIe.MUX_SEL`).  See
[`../v2n-m1/`](../v2n-m1/) for the overlay.
