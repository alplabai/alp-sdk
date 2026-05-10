# E1M EVK — SDK reference

The **E1M Development Board** (UG-E1M-001) is the official ALP Lab
carrier board for E1M / E1M-X System-on-Modules.  It exposes USB,
Ethernet, CAN, MIPI DSI, multiple camera options, audio, sensors,
M.2/PCIe, and Arduino + mikroBUS expansion — all wired so that any
E1M-conformant SoM (E1M-AEN family, E1M-X V2N, etc.) plugs into the
same carrier and the same SDK build runs.

> Source: *E1M Development Board User Guide* — UG-E1M-001 Rev. 0.1
> (April 2026), shipped under
> `OneDrive/E1M Project/E1M Development Board/80 - Released Documentation/EVK User Guide/`.

This file is the **SDK-side cheat sheet** for the EVK: bus map,
sensor I²C addresses, button/LED assignments, IO-expander, and the
bring-up checklist most relevant to firmware writers.  The Zephyr
board file (`boards/<vendor>/alp_e1m_evk*`) lives in
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
(per [`docs/architecture.md`](../architecture.md), board files are
not in this repo).

## SoM compatibility

| SoM family       | EVK support | Notes                                                                                  |
|------------------|-------------|----------------------------------------------------------------------------------------|
| E1M-AEN (Alif Ensemble) | **v0.1** target | Primary bring-up target. ETH0 only (AEN family routes a single MAC).             |
| E1M-X V2N        | v0.2        | Two MAC controllers wired to the EVK's two RJ45 jacks.                                 |
| E1M-X V2N-M1     | v0.3        | DX-M1 sits behind PCIe; M.2 Key M slot is functionally a bridge share, not exclusive ×4. |

## Power and rails

- **Primary power input:** barrel jack `+V_BRL`, 7 V – 15 V.
- **Secondary inputs:** USB-C J12 / J13 (each carries `USB1_VBUS` /
  `USB2_VBUS` and CC-pin straps).  Don't drive multiple inputs
  simultaneously during early bring-up — the power-OR / eFuse
  topology hasn't been verified on every assembled revision.
- **Internal rails:** `+5V`, `+3V3`, `+1V8`, `+VIO` (selectable
  +1V8 / +V_ANA / +3V3 / +5V via header **P17** for the user
  interface and Arduino expansion).
- **SuperCap rail:** present on `+SCAP`; useful for hold-up during
  brown-outs; `<alp/iot.h>`-level state-persistence policies should
  consult this rail when shipping examples that survive power loss.
- **PG signals:** `5V_PG`, `3V3_PG`, `1V8_PG` are exposed at test
  points and (some) on the IO expander.

## Boot, reset, debug

| Item              | Where                         | SDK interaction                                   |
|-------------------|-------------------------------|---------------------------------------------------|
| JTAG / SWD        | J2 (FTSH-105, 10-pin)         | Used by `west flash` / `west debug`.              |
| Reset button      | Tied to `PORn` on the module  | Hard reset; bypasses any SDK lifecycle.           |
| BOOT0..BOOT3 DIP  | SW1                           | Per-SKU boot mode.  E1M-AEN family ignores these (boot pads are NC on AEN per `vendors/alif/README.md`). |
| Module enable     | Header P12 (short to disable) | Drives `MODULE_EN` low → SoM stays off.           |
| Antenna           | U.FL connector                | The on-module Wi-Fi 6 + BLE 5.4 combo connects here. |

## I²C bus map (`alp_i2c0`)

The EVK shares **one I²C bus** (`I2C0.SCL` / `I2C0.SDA`, pulled up
4.7 kΩ to `+VIO`) across the on-board sensors, two TCAL9538 I/O
expanders, the INA236 current monitors, and the two TAS2563 smart
amplifiers.  All addresses below are confirmed against the EVK
schematic (UG-E1M-001) and exposed as `EVK_I2C_ADDR_*` macros in
[`<alp/boards/alp_e1m_evk.h>`](../../include/alp/boards/alp_e1m_evk.h).

| Address (7-bit) | Device                  | Macro                              | Role                                                |
|-----------------|-------------------------|------------------------------------|-----------------------------------------------------|
| `0x40`          | INA236A (U21)            | `EVK_I2C_ADDR_INA236_3V3`          | `+3V3` rail current monitor (20 mΩ shunt, 4.0 A max) |
| `0x41`          | INA236A (U31)            | `EVK_I2C_ADDR_INA236_1V8`          | `+1V8` rail current monitor (20 mΩ shunt, 4.0 A max) |
| `0x42`          | INA236A (U33)            | `EVK_I2C_ADDR_INA236_VIO`          | `+VIO` rail current monitor (50 mΩ shunt, 1.6 A max) |
| `0x47`          | BMP581 (U14)             | `EVK_I2C_ADDR_BMP581`              | Barometric pressure (SDO=1)                         |
| `0x48`          | INA236B (U32)            | `EVK_I2C_ADDR_INA236_VCAM0`        | `+V_CAM0` rail current monitor (50 mΩ shunt, 1.6 A max) |
| `0x49`          | INA236B (U34)            | `EVK_I2C_ADDR_INA236_VCAM1`        | `+V_CAM1` rail current monitor (50 mΩ shunt, 1.6 A max) |
| `0x4A`          | INA236B (U30)            | `EVK_I2C_ADDR_INA236_5V`           | `+5V` rail current monitor (20 mΩ shunt, 4.0 A max)  |
| `0x4D`          | TAS2563 (U27)            | `EVK_I2C_ADDR_TAS2563_LOW`         | Smart-amp #1 (AD0 = 10 kΩ to GND)                   |
| `0x4E`          | TAS2563 (U28)            | `EVK_I2C_ADDR_TAS2563_HIGH`        | Smart-amp #2 (AD0 = 10 kΩ to VDD)                   |
| `0x68`          | BMI323 (U13)             | `EVK_I2C_ADDR_BMI323`              | Secondary 6-axis IMU (SDO=0; no collision with ICM) |
| `0x69`          | ICM-42670-P (U12)        | `EVK_I2C_ADDR_ICM42670`            | Primary 6-axis IMU (AD0=1)                          |
| `0x71`          | TCAL9538 PCIe (U37)      | `EVK_I2C_ADDR_TCAL9538_PCIE`       | PCIe-side I/O expander (PCIe slot RST/WAKE/CLKREQ)  |
| `0x72`          | TCAL9538 main (U35)      | `EVK_I2C_ADDR_TCAL9538_MAIN`       | Main I/O expander (LCD/cam/CTP control + IMU IRQs)  |

> **TMUX121 is not in this table.**  It's a passive analog/digital
> I²C bus switch — addressless, controlled via dedicated pins
> (`PCIE0_I2C.EN` from E1M `IO10`, `PCIE0_I2C.SEL` from the PCIe
> TCAL9538).  When the mux is disabled, the downstream PCIe-side
> I²C bus is isolated from `alp_i2c0` entirely.

The I²C bus is shared between **sensors and current monitors** — when
profiling power, code that reads ICM-42670-P at high rate will
contend with INA236 reads.  The SDK's `examples/profile-power-aen/`
will demonstrate the recommended interleaving.

### Sensor interrupts and IO-expander mux

The TCAL9538 IO expander (U35) drives:

| Expander pin  | Net           | Drives                                              |
|---------------|---------------|-----------------------------------------------------|
| `LCD_PWR_EN`  | output        | Display 1V8 / 3V3 enable                            |
| `LCD_RST`     | output        | Display panel reset                                 |
| `CTP_RST`     | output        | Capacitive touch reset                              |
| `CAM_EN`      | output        | Camera-module enable (drives the camera sensor's EN/STBY pin, NOT the camera power rails) |
| `S_42670.INT1`| input         | ICM-42670-P interrupt 1                             |
| `S_42670.INT2`| input         | ICM-42670-P interrupt 2                             |
| `S_42670.FSYNC`| input        | ICM-42670-P FSYNC                                   |
| `S_BMP581.INT1`| input        | BMP581 interrupt                                    |

The expander itself signals back via `IO_EXP.INT` (interrupt out)
and is reset via `IO_EXP.RST`.  Both are routed to the module.

## User interface

| Item             | Description                                                             |
|------------------|-------------------------------------------------------------------------|
| Rotary encoder   | PEC12R-4222F-S0024 — quadrature on `ENC0_X`/`ENC0_Y` plus an integrated push switch. |
| RGB LED          | 150505M173300, transistor-driven, on a `+5V` rail.                      |
| DAC outputs      | `DAC0_OUT` and `DAC1_OUT` buffered through OPA189 op-amps to header J15.|
| Comparator       | `CMP0`, `CMP1` exposed on header J18.                                   |
| IO-voltage select| Header **P17**: jumper between `+1V8`, `+V_ANA`, `+3V3`, `+5V`.         |

## Networking & I/O at a glance

- **Ethernet:** two RJ45 MagJacks (ARJM11C7-502-KB-EW2) wired to
  `ETH0_*` and `ETH1_*`.  E1M-AEN populates only ETH0 — the second
  jack stays dark.  Each jack carries the standard activity LEDs
  (`ETH*_LED0`, `ETH*_LED1`).
- **CAN bus:** TCAN1044A transceiver, jumpers JP1–JP4, header J9.
- **microSD:** standard slot multiplexed via 74LVC157 with the M.2
  Key E SDIO interface — software must pick which one is active.
- **Camera:** three options — Raspberry-Pi-compatible 15-pin CSI,
  standard MIPI B2B 34-pin, parallel DVP 24-pin — multiplexed via
  the **PI3WVR626XEBEX** 2:1 MIPI CSI mux.  Camera rails
  `+1V2_CAM`, `+2V6_CAM` are feedback-resistor-tunable.

  The mux's `SEL` pin is driven by **`CAM_MUX.SEL`**, which the
  EVK schematic ties to **E1M `IO2` (`W2`, Alif `P12.5`)**.  Per
  the PI3WVR626 truth table:

      SEL = 0  -> input A (`A_MIPI_CSI_*`) routes to the SoM
      SEL = 1  -> input B (`B_MIPI_CSI_*`) routes to the SoM

  Use `EVK_PIN_CAM_MUX_SEL` from
  `<alp/boards/alp_e1m_evk.h>` plus the
  `cam_mux_pi3wvr626_*` helper in `<alp/chips/cam_mux_pi3wvr626.h>`
  to switch inputs at runtime.  The `/OE` pin is hardwired to GND
  on this board, so the output is always live.

  > **Important.**  E1M `IO2` was previously documented as the RGB
  > LED-blue channel.  That was a placeholder guess; the EVK
  > schematic confirms `IO2` is the camera mux SEL line.  The
  > actual blue-LED pin is TBD until the user confirms the LED
  > matrix wiring.
- **Audio:** two PDM microphones (MP34DT05TR-A) and two TAS2563
  Class-D amps (U27 + U28; each drives a mono speaker) with JST
  speaker headers; I²S source selectable via 74LVC157.  The shared
  `I²S0` link carries both channels in a stereo frame -- U27 picks
  the left slot, U28 picks the right slot via TAS2563 time-slot
  configuration.  The amps' diagnostic feedback returns on
  `I²S0_SDI`.
- **Expansion:** Arduino headers + mikroBUS click headers, level-shifted
  through LSF0108 / LSF0102 to the IO-voltage select rail.
- **PCIe / M.2:** Key M and Key E with PI3DBS12212A lane mux,
  SY75602 refclk buffer, **TMUX121NKGR** passive I²C mux
  (pin-controlled, no I²C address), and a second TCAL9538 (`0x71`)
  for the PCIe-side resets/WAKE/CLKREQ signals.
- **Display:** 40-pin MIPI DSI connector for the **RK055HDMIPI4MA0**
  720p panel (NXP-supplied reference panel; drivers are available
  from NXP's MIPI-DSI panel collection).  Backlight rails + the
  capacitive-touch controller sit on `EVK_AEN_I2C_BUS_DSI_CSI`
  (`E1M_I2C1`).
- **Rotary encoder phase pads:** `ENC0_X` (A) and `ENC0_Y` (B) for
  the PEC12R-4222F-S0024 quadrature signals.  The push-switch
  (SW) is on E1M `IO4` -- `EVK_PIN_ENCODER_SW`.

## Bring-up checklist (firmware perspective)

1. Visual inspection — solder bridges around the module footprint,
   M.2 connectors, fine-pitch camera / display FFCs.
2. Power-only test (no peripherals) — apply barrel within 7–15 V,
   verify rail LEDs (`+3V3`, `+1V8`, `+VIO`, `USB_HOST_VBUS`),
   probe `5V_PG` / `3V3_PG` / `1V8_PG` test points.
3. Confirm `MODULE_EN` not held low (P12 jumper *off*).
4. Set `BOOT0..BOOT3` on SW1 per the SoM's expected boot mode.
   On AEN, leave them — the SoM ignores them.
5. SWD/JTAG: connect to J2.  Confirm the IO reference voltage is
   set on the header (`+VIO`).
6. Add peripherals one at a time — Ethernet → microSD → display →
   camera → M.2 modules.

## Known design notes (to track)

- Power sheet: "Check voltage division — boots at 13 V."  Track in
  EVK errata before production.
- Boot sheet: pull-up / pull-down for boot pins must live in the
  module, not on the EVK.

## What this means for the SDK

- v0.1 ships an **EVK overlay** under `tests/zephyr/peripheral/boards/`
  that wires `alp_i2c0`, the `alp,pin-array` (rotary encoder, RGB
  LED, IO_EXP.INT), and `alp_uart0` to EVK pins via the SoM's
  pinmux.  It targets `alp_e1m_evk_aen` (AEN-family build); V2N
  builds add their own overlay in v0.2.
- v0.1 does **not** ship full board-level sensor drivers.  The
  ICM-42670-P / BMI323 / BMP581 / TCAL9538 drivers land as part of
  the v0.2 "Chips" library expansion (`chips/icm42670p/`, etc.) per
  [`VERSIONS.md`](../../VERSIONS.md).
- The EVK example app (`examples/evk-bringup/`) lands in v0.2.  v0.1
  ships a stub README at that path so the doc tree is stable.

## See also

- [`vendors/alif/README.md`](../../vendors/alif/README.md) — Alif
  Ensemble HAL pin (E1M-AEN family caveats: ETH0 only, no CSI1, etc.).
- [`docs/architecture.md`](../architecture.md) — full SDK layering.
- [`VERSIONS.md`](../../VERSIONS.md) — when each EVK feature
  becomes GA.
