# E1M EVK — SDK reference

The **E1M Development Board** (UG-E1M-001) is the official Alp Lab
board for the **E1M** (35 × 35 mm) form factor.  It exposes USB,
Ethernet, CAN, MIPI DSI, multiple camera options, audio, sensors,
M.2/PCIe, and Arduino + mikroBUS expansion — all wired so that any
E1M-conformant SoM (the E1M-AEN family today; E1M-N93 once its HW
config lands) plugs into the same board and the same SDK build
runs.  E1M-X SoMs (Renesas RZ/V2N, V2N-M1 — 45 × 65 mm, a separate
product line with its own C namespace) do **not** fit this board;
they use the [E1M-X Development Board](e1m-x-evk.md) instead.

> Source: *E1M Development Board User Guide* — UG-E1M-001 Rev. 0.1
> (April 2026), archived vendor documentation
> tree.

This file is the **SDK-side cheat sheet** for the EVK: bus map,
sensor I²C addresses, button/LED assignments, IO-expander, and the
bring-up checklist most relevant to firmware writers.  The Zephyr
board files for the AEN family SoMs on this EVK
(`alp_e1m_aen801_m55_he`, `alp_e1m_aen801_m55_hp`,
`alp_e1m_aen401_m55_hp`, `alp_e1m_aen601_m55_hp`) ship in-tree at
[`zephyr/boards/alp/`](../../zephyr/boards/alp/) (per
[`docs/architecture.md`](../architecture.md); there is no separate
board-file repo).

## SoM compatibility

This EVK accepts **E1M-conformant SoMs only** — the E1M and E1M-X
form factors are separate product lines (different pad counts,
different C namespaces; no cross-form-factor portability) and do
not share a board.

| SoM family       | EVK support | Notes                                                                                  |
|------------------|-------------|----------------------------------------------------------------------------------------|
| E1M-AEN (Alif Ensemble) | **v0.1** target | Primary bring-up target. ETH0 only (AEN family routes a single MAC).             |
| E1M-N93 (NXP i.MX 93)   | planned, no committed version | `VERSIONS.md` Tier 3 ("deferred indefinitely past v1.0") lists NXP NX9101 silicon enablement.  Provisional preset `E1M-NX9101`; production MPN pending the HW config writeup. |

E1M-X SoMs (`E1M-V2N101/102`, `E1M-V2M101/102`) target the separate
[E1M-X Development Board](e1m-x-evk.md), not this one.

## Power and rails

- **Primary power input:** barrel jack `+V_BRL`, 7 V – 15 V.
- **Secondary inputs:** USB-C J12 / J13 (each carries `USB1_VBUS` /
  `USB2_VBUS` and CC-pin straps).  Don't drive multiple inputs
  simultaneously during early bring-up — the power-OR / eFuse
  topology hasn't been verified on every assembled revision.
- **Internal rails:** `+5V`, `+3V3`, `+1V8`, `+VIO` (the plugged-in
  SoM's `VIO_OUT`, NOT carrier-selectable -- measures 1.8 V with the
  E1M-AEN SoM), `+V_ANA` (jumper-selectable from `+5V` / `+3V3` /
  `+1V8` via header **P17**; feeds only DAC header J15 and the
  OPA189 DAC buffers U23/U24 -- NOT the Arduino/mikroBUS expansion),
  `+VARD` (same jumper shape on header **P9**) -- not only the
  Arduino/mikroBUS expansion: it feeds the LSF0108 level shifters'
  `VREF_B` side through R176/R177/R178 (`VREF_A` on those shifters is
  `+VIO`), and directly supplies Arduino UART header J17 pin 1,
  encoder header J18 pin 1, header P3 pin 2, the `CK_SCL`/
  `CK_SDA` pull-ups R146/R147, U16/U17's `REFB` through R173/R174, and
  a pull-up on `CK_RST` through R175 (`CK_RST` reaches mikroBUS
  header P7 pin 2, P3 pin 3, and U17 pin B1).
  Setting P9 to `+5V` puts 5 V on J17, J18, `CK_RST`, and those I2C
  pull-ups too, not just the Arduino/mikroBUS shifters.
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

## I²C bus map (`alp-i2c0`)

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
| `0x49`          | INA236B (U34)            | `EVK_I2C_ADDR_INA236_VCAM1`        | `+V_CAM1` rail current monitor (50 mΩ shunt, 1.6 A max) |
| `0x4A`          | INA236B (U30)            | `EVK_I2C_ADDR_INA236_5V`           | `+5V` rail current monitor (20 mΩ shunt, 4.0 A max)  |
| `0x4B`          | INA236B (U32)            | `EVK_I2C_ADDR_INA236_VCAM0`        | `+V_CAM0` rail current monitor (50 mΩ shunt, 1.6 A max).  Re-strapped A0=SCL → `0x4B` **from the next batch**; PRE-RESPIN boards had it at `0x48`, colliding with the TAS2563 broadcast address (unreadable there). |
| `0x4D`          | TAS2563 (U27)            | `EVK_I2C_ADDR_TAS2563_LOW`         | Smart-amp #1 (AD0 = 10 kΩ to GND)                   |
| `0x4E`          | TAS2563 (U28)            | `EVK_I2C_ADDR_TAS2563_HIGH`        | Smart-amp #2 (AD0 = 10 kΩ to VDD)                   |
| `0x68`          | BMI323 (U13)             | `EVK_I2C_ADDR_BMI323`              | Secondary 6-axis IMU (SDO=0; no collision with ICM) |
| `0x69`          | ICM-42670-P (U12)        | `EVK_I2C_ADDR_ICM42670`            | Primary 6-axis IMU (AD0=1)                          |
| `0x71`          | TCAL9538 PCIe (U37)      | `EVK_I2C_ADDR_TCAL9538_PCIE_NOT_ASSEMBLED` | **NOT ASSEMBLED** on this EVK revision (alp-sdk#1974) -- would be the PCIe-side I/O expander (PCIe slot RST/WAKE/CLKREQ). The generator (#1980) renames the macro so the plain `EVK_I2C_ADDR_TCAL9538_PCIE` name is not defined. |
| `0x73`          | TCAL9538 main (U35)      | `EVK_I2C_ADDR_TCAL9538_MAIN`       | Main I/O expander (LCD/cam/CTP control + IMU IRQs). CORRECTED 2026-09-05 from `0x72` (alp-sdk#1974): the maintainer's EVK I2C schedule gives 1110011 = `0x73`, and 2 of 2 boards answer there and are silent at `0x72`. |

> **TMUX121 is not in this table.**  It's a passive analog/digital
> I²C bus switch — addressless, controlled via dedicated pins
> (`PCIE0_I2C.EN` from E1M `IO10`, `PCIE0_I2C.SEL` from the PCIe
> TCAL9538).  When the mux is disabled, the downstream PCIe-side
> I²C bus is isolated from `alp-i2c0` entirely.

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
| `CAM_EN`      | output        | Camera-module enable line, NOT the camera power rails. On the RPi connector (J5) it acts on pin 11 through an inverting open-drain stage: `0` (reset default) leaves pin 11 floating so the module self-enables on its own pull-up; `1` pulls pin 11 low and powers the module down. Leave it at `0` for RPi modules. |
| `S_42670.INT1`| input         | ICM-42670-P interrupt 1                             |
| `S_42670.INT2`| input         | ICM-42670-P interrupt 2                             |
| `S_42670.FSYNC`| input        | ICM-42670-P FSYNC                                   |
| `S_BMP581.INT1`| input        | BMP581 interrupt                                    |

The expander itself signals back via `IO_EXP.INT` (interrupt out)
and is reset via `IO_EXP.RST`.  Both are routed to the module.

## User interface

| Item             | Description                                                             |
|------------------|-------------------------------------------------------------------------|
| Rotary encoder   | PEC11R-4215K-S0024 — quadrature on `ENC0_X`/`ENC0_Y` plus an integrated push switch. |
| RGB LED          | 150505M173300, transistor-driven, on a `+5V` rail.                      |
| DAC outputs      | `DAC0_OUT` and `DAC1_OUT` buffered through OPA189 op-amps to header J15.|
| Comparator       | `CMP0`/`CMP1` are not broken out on a dedicated header.                 |
| `+V_ANA` select  | Header **P17**: jumper connects one of `+5V`/`+3V3`/`+1V8` (pins 1/3/5) to `+V_ANA` (pins 2/4/6); feeds only DAC header J15 and the OPA189 DAC buffers -- does not reach the Arduino/mikroBUS expansion or `+VIO`, which comes from the SoM's `VIO_OUT`. |
| `+VARD` select   | Header **P9**, same shape as P17 (pins 1/3/5 = `+5V`/`+3V3`/`+1V8`, pins 2/4/6 = `+VARD`). NOT only the Arduino/mikroBUS expansion rail: sets `VREF_B` on the LSF0108 level shifters U18/U22/U40 (through R176/R177/R178, `VREF_A` = `+VIO`), and directly supplies Arduino UART header J17 pin 1, encoder header J18 pin 1, header P3 pin 2, the `CK_SCL`/`CK_SDA` pull-ups R146/R147 and U16/U17 `REFB` (through R173/R174), and a pull-up on `CK_RST` through R175 (`CK_RST` reaches mikroBUS header P7 pin 2, P3 pin 3, and U17 pin B1). Setting P9 to `+5V` puts 5 V on all of those, not just the shifters. |

## Networking & I/O at a glance

- **Ethernet:** two RJ45 MagJacks (ARJM11C7-502-KB-EW2) wired to
  `ETH0_*` and `ETH1_*`.  E1M-AEN populates only ETH0 — the second
  jack stays dark.  Each jack carries the standard activity LEDs
  (`ETH*_LED0`, `ETH*_LED1`).
- **CAN bus:** TCAN1044A transceiver, jumpers JP1–JP4, header J9.
- **microSD:** standard slot multiplexed via a 2:1 mux (U38/U39) with
  the M.2 Key E SDIO interface. As-built, U38/U39 are stock 74LVC157
  and can NEVER pass SD through at any `VCC` -- they contend with the
  SoC's own host controller in BOTH `/E` states (same directional
  failure as the I2S mux, U46); a 3257-type bus-switch swap is
  required (see `include/alp/boards/alp_e1m_evk.h`). The select is
  hardware-strapped on r2 (not software-drivable — the default is the
  microSD slot); r1 can drive it from firmware over the CC3501E GPIO
  proxy, but doing so while header P18's jumper is fitted is a
  hardware hazard. **Disabled on 2626-R2 (#2051):** `sdhc0` stays
  `status = "disabled"` in the shared SoC dtsi rather than fight the
  mux's held-low/contending pads with the SoC's own drivers; see
  `examples/aen/aen-sdhc-probe`'s README.
- **Camera:** three options — Raspberry-Pi-compatible 15-pin CSI,
  standard MIPI B2B 34-pin, parallel DVP 24-pin — multiplexed via
  the **PI3WVR626XEBEX** 2:1 MIPI CSI mux.  Camera rails
  `+1V2_CAM`, `+2V6_CAM` are feedback-resistor-tunable.

  The mux's `SEL` pin is driven by `CAM_MUX.SEL`, which the
  EVK schematic ties to E1M `IO2` (`W2`, Alif `P12.5`).  Per
  the PI3WVR626 truth table:

      SEL = 0  -> input A (`A_MIPI_CSI_*`) routes to the SoM
      SEL = 1  -> input B (`B_MIPI_CSI_*`) routes to the SoM

  Use `EVK_PIN_CAM_MUX_SEL` from
  `<alp/boards/alp_e1m_evk.h>` plus the
  `cam_mux_pi3wvr626_*` helper in `<alp/chips/cam_mux_pi3wvr626.h>`
  to switch inputs at runtime.  The `/OE` pin is hardwired to GND
  on this board, so the output is always live.

  **Raspberry Pi camera connector (J5).**  J5 is the RPi 15-pin
  CSI-2 connector and sits on mux input **A** (`SEL = 0`).  It
  carries **2 data lanes** (the most the 15-pin pinout has), no reset
  line and no sensor clock -- RPi modules carry their own
  oscillator.  Its control I2C is E1M `I2C1` (`EVK_I2C_BUS_DSI_CSI`
  = SoC I2C1, SCL `P3_7` / SDA `P7_2`), level-shifted to 3.3 V on the
  carrier and shared with the DSI connector's touch controller.  Pin
  11 (module enable) is driven by `CAM_EN` (see the expander table
  above): keep `CAM_EN = 0` so the module self-enables.

  On an E1M-AEN SoM, build a camera app with the board-side shield
  `e1m_evk_rpi_csi` paired with a sensor shield that follows
  Zephyr's Raspberry Pi camera contract, e.g. the Camera Module 2
  (IMX219):

      west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <app> -- \
        -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_2"

  `e1m_evk_rpi_csi` wires J5 to the E8's dedicated CSI-2 receive
  D-PHY, hogs `IO2` low (input A), enables SoC I2C1 as the sensor
  bus, and points `alp-camera0` / `zephyr,camera` at the CPI.  The
  SoC side lives in the shield's per-target overlays
  (`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` and
  the AEN803 twin, both including `boards/e1m_aen.dtsi`), so the
  shield builds for the AEN801 and AEN803 M55-HE targets.  The D-PHY
  clocks are the SoC `dphy` node's four real `MIPI_CKEN` gates, so
  the shield composes with a DSI panel app without either overriding
  them.  What feeds those gates is also the D-PHY driver's job: at
  init `dphy_dw.c` enables the CGU `CLK_ENA` HFOSC (38.4 MHz PLL
  reference) and 100 MHz (CFG clock) sources and clears the VBAT
  `PWR_CTRL` D-PHY power masks, isolation and 1.8 V bypass.  At reset
  those leave the D-PHY unpowered, and the CSI-2 receiver then times
  out waiting for Stop-state.  The sensor shields and their drivers are described in
  [`docs/camera-shields.md`](../camera-shields.md).  The
  app needs `CONFIG_ALP_SDK=y` and `CONFIG_VIDEO=y`, and a video
  buffer pool that fits in RAM (the Zephyr 2 MB default does not fit
  the HE core's DTCM).  RAW10 sensors are delivered to memory as
  unpacked 16-bit samples (`VIDEO_PIX_FMT_SBGGR10`, pitch = width x 2),
  not the packed wire format.  Compiled against the upstream IMX219
  driver; not yet run on hardware.

  The CSI-2 pixel clock tops out at 200 MHz (400 MHz source / 2), and
  one pixel moves per clock.  A 2-lane IMX219 at its 456 MHz link
  therefore needs RAW10 (`VIDEO_PIX_FMT_SBGGR10P`, 182.4 Mpixel/s):
  RAW8 would need 228 Mpixel/s and `video_set_format()` refuses it
  with `-ERANGE`.  The divider is programmed by the Alif clock-control
  patch in `zephyr/patches.yml`, so the workspace must be patched
  (`scripts/bootstrap.sh` does it).

  [`examples/aen/aen-camera-firstlight`](../../examples/aen/aen-camera-firstlight/)
  is the bench first-light app for this connector: it opens each of
  the four shipped camera shields (IMX219 / OV5647 / OV9281 / IMX296)
  through `<alp/camera.h>`, starts the
  stream, and waits for one frame with a 2 s timeout, printing a CRC32
  + histogram + sample row bytes on success or a diagnosed failure
  otherwise. See its README for what each printed line means.

  > **Important.**  E1M `IO2` was previously documented as the RGB
  > LED-blue channel.  That was a placeholder guess; the EVK
  > schematic confirms `IO2` is the camera mux SEL line.  The
  > actual blue-LED pin is TBD until the user confirms the LED
  > matrix wiring.
- **Audio:** two PDM microphones (MP34DT05TR-A) and two TAS2563
  Class-D amps (U27 + U28; each drives a mono speaker) with JST
  speaker headers, reachable over `I²S0` only through a mux (U46).
  As originally built, U46 is a `74LVC157ABQ,115` -- its `VCC` range
  (1.2-3.6 V) makes `+VIO` at 1.8 V IN SPEC, but the part is a
  ONE-WAY mux whose `Y` outputs drive the SoC side, so it can NEVER
  pass SoC-to-amp I2S at any `VCC`; moving its `VCC` to `+3V3` alone
  does NOT make it work, and re-enabling `i2s3` on a stock board puts
  the SoC's own I2S3 TX in contention with U46's driven outputs.  A
  working U46 needs BOTH a 3257-type bus-switch swap AND `VCC` on
  `+3V3` -- on `e1m-aen-evk-03` (2026-09-15), a fitted 3257-type
  part's `VCC` was moved to `+3V3` between a silent run and an
  audible run; not established as the only difference between the
  two -- or a switch rated for 1.8 V `VCC` (untested).  Only
  amp playback audibility was checked this way; PDM mic capture and
  M.2 E-key I2S are unverified, disabling the mux or selecting M.2
  (`/E`/`S` HIGH) may not switch reliably at `+3V3`, and the same run
  showed an open TDM clock-error latch during playback plus an open
  amp auto-shutdown-after-stop issue (#2146) -- see
  `include/alp/boards/alp_e1m_evk.h`'s I2S mux block for the full
  finding.  The shared `I²S0` link is designed to carry both channels
  in a stereo frame -- U27 the left slot, U28 the right, via TAS2563
  time-slot configuration.  The amps' diagnostic feedback returns on
  `I²S0_SDI`.
- **Expansion:** Arduino headers + mikroBUS click headers, level-shifted
  through LSF0108 / LSF0102 to `+VARD`, the header-**P9**-selectable
  expansion IO rail (NOT `+V_ANA`/P17, which only feeds the DAC path).
- **PCIe / M.2:** Key M and Key E with PI3DBS12212A lane mux,
  SY75602 refclk buffer, **TMUX121NKGR** passive I²C mux
  (pin-controlled, no I²C address), and a second TCAL9538 (`0x71`)
  for the PCIe-side resets/WAKE/CLKREQ signals.
- **Display:** 40-pin MIPI DSI connector for the **RK055HDMIPI4MA0**
  720p panel (NXP-supplied reference panel; drivers are available
  from NXP's MIPI-DSI panel collection).  Backlight rails + the
  capacitive-touch controller sit on `EVK_I2C_BUS_DSI_CSI`
  (`ALP_E1M_I2C1`).
- **Rotary encoder phase pads:** `ENC0_X` (A) and `ENC0_Y` (B) for
  the PEC11R-4215K-S0024 quadrature signals.  The push-switch
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
  that wires the `alp-i2c0` alias, the `alp,pin-array` (rotary encoder,
  RGB LED, IO_EXP.INT), and the `alp-uart0` alias to EVK pins via the
  SoM's pinmux.  It targets `alp_e1m_aen801_m55_he` (AEN-family build);
  future E1M-N93 builds add their own overlay once that SoM lands.
- v0.1 does **not** ship full board-level sensor drivers.  The
  ICM-42670-P / BMI323 / BMP581 / TCAL9538 drivers land as part of
  the v0.2 "Chips" library expansion (`chips/icm42670/`, etc.) per
  [`VERSIONS.md`](../../VERSIONS.md).
- The EVK example app (`examples/evk-bringup/`) lands in v0.2.  v0.1
  ships a stub README at that path so the doc tree is stable.

## See also

- [`vendors/alif/README.md`](../../vendors/alif/README.md) — Alif
  Ensemble HAL pin (E1M-AEN family caveats: ETH0 only, no CSI1, etc.).
- [`docs/architecture.md`](../architecture.md) — full SDK layering.
- [`VERSIONS.md`](../../VERSIONS.md) — when each EVK feature
  becomes GA.
