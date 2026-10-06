# E1M-X Development Board — SDK reference

Board for the **E1M-X** form factor (45 × 65 mm) — hosts
the Renesas RZ/V2N family (`E1M-V2N101`, `E1M-V2N102`, `E1M-V2N103`,
`E1M-V2M101`, `E1M-V2M102`, `E1M-V2M103`) and any future E1M-X conformant SoM.

> Source: vendor datasheet
> — Altium project (multi-sheet schematic).  No standalone user
> guide on file yet; this doc captures what's discoverable from
> the schematic-sheet inventory until the user writes the
> authoritative HW configuration.

## Status

**v0.2+ deliverable.**  This page is a structural placeholder.
Per the project memory note "pending exact hardware
configurations," concrete pin assignments / I²C addresses / boot
strap settings land when the user supplies them.

## Schematic-sheet inventory (per the Altium project)

The E1M-X dev board V1 schematic is split across the following
sheets — each is a discoverable feature block on the board:

| Sheet                          | Block                                                         |
|--------------------------------|---------------------------------------------------------------|
| `E1M-X Interface 1.SchDoc`     | E1M-X 256-pad interface — half 1.                             |
| `E1M-X Interface 2.SchDoc`     | E1M-X 256-pad interface — half 2.                             |
| `Boot and Debug.SchDoc`        | JTAG/SWD header, BOOT-mode straps, reset/enable.              |
| `Camera 1.SchDoc`              | First MIPI CSI / parallel camera connector.                   |
| `Camera 2.SchDoc`              | Second MIPI CSI / parallel camera connector (E1M-X has 2× CSI). |
| `Display 1.SchDoc`             | First MIPI DSI display path.                                  |
| `Display 2.SchDoc`             | Second MIPI DSI display path (E1M-X has 2× DSI lane sets).    |
| `Ethernet - SD.SchDoc`         | 2× RJ45 + microSD socket.                                     |
| `CAN-BUS.SchDoc`               | CAN transceivers (likely 2× CAN-FD).                          |
| `Mikro BUS.SchDoc`             | mikroBUS click-board expansion header(s).                     |
| `Current Measurement.SchDoc`   | Per-rail INA236 monitors.                                     |

Both display FFCs are wired point-to-point (ESD clamps + LSF0102 level
shifters only).  Datasheets for the ITE IT6162 and Diodes PI3WVR648 exist
in the vendor datasheet folder but **neither part is populated** on the
E1M-X EVK.

## Compared to the E1M EVK (35 × 35)

The E1M-X dev board is the larger sibling of the E1M EVK
(UG-E1M-001, see [`e1m-evk.md`](e1m-evk.md)).  Anticipated
deltas (verify when the HW config writeup lands):

| Feature                  | E1M EVK (35 × 35)                | E1M-X EVK (45 × 65)                |
|--------------------------|----------------------------------|------------------------------------|
| SoM form factor          | E1M (312 pads)                   | E1M-X (496 pads)                   |
| Ethernet jacks           | 2× RJ45 (only ETH0 routed on AEN)| 2× RJ45 (both routed on V2N family)|
| Camera connectors        | RPi-CSI + MIPI B2B + parallel DVP| 2× MIPI CSI (separate connectors)  |
| Display                  | 1× MIPI DSI 40-pin               | 2× MIPI DSI                        |
| M.2 slots                | Key M + Key E                    | Key M + Key E (sized for V2N+M1)   |
| Expansion                | Arduino + mikroBUS               | mikroBUS (Arduino TBD)             |
| AI accelerator option    | n/a (E1M-AEN's NPUs are on-die)  | DEEPX DX-M1 plug-in (V2N-M1 path)  |

## Targeted SoM SKUs

| SoM SKU       | Backing silicon                                          | Module metadata `silicon` |
|---------------|----------------------------------------------------------|---------------------------|
| `E1M-V2N101`  | Renesas `R9A09G056N44GBG#AC0`                            | `renesas:rzv2n:n44`       |
| `E1M-V2N102`  | Renesas `R9A09G056N44GBG#AC0` (different memory tier)    | `renesas:rzv2n:n44`       |
| `E1M-V2N103`  | Same, alt memory tier (4 GB / 16 GB)                     | `renesas:rzv2n:n44`       |
| `E1M-V2M101`  | Renesas `R9A09G056N44GBG#AC0` + DEEPX `DX-M1`            | `renesas:rzv2n:n44` (+ `npu: deepx_dxm1`) |
| `E1M-V2M102`  | Same, alt memory tier                                    | same                      |
| `E1M-V2M103`  | Same, alt memory tier (4 GB / 16 GB)                     | same                      |

## What this means for the SDK

- v0.2 ships first-class support for the E1M-X EVK + V2N101 SoM via
  the in-tree Zephyr board definition below -- there is no separate
  per-SoM peripheral-test overlay or placeholder header under
  `include/alp/boards/` for V2N101 (unlike the AEN EVK's
  `alp_e1m_evk.h` / `alp_e1m_evk_routes.h`); the carrier-level
  `alp_e1m_x_evk.h` / `alp_e1m_x_evk_routes.h` pair covers it.
- v0.3 extends to V2M101 / V2M102 with DX-M1 detection on PCIe.
- The Zephyr board file for `alp_e1m_v2n101_m33_sm` ships in-tree at
  [`zephyr/boards/alp/e1m_v2n101_m33_sm/`](../../zephyr/boards/alp/e1m_v2n101_m33_sm/)
  — no separate board-file repo, same as the AEN EVK.

## Display

Per-path status and bench record: [display-support-matrix.md](../display-support-matrix.md).

### Display 1 (J6) — V2N primary display path

| Item | Detail |
|------|--------|
| Connector | J6 — DSI0 lane set (4 data pairs wired to the E1M-X SoM) |
| Panel | Rocktech RK055HDMIPI4MA0 — 5.5″ 720×1280, Himax HX8394-F controller |
| Link config | 2-lane MIPI-DSI, RGB565 (per the NXP / Rocktech reference configuration) |
| Backlight | SoM-side PWM exposed to Linux as a `pwm-backlight` device tree node; 5 kHz PWM. |
| Panel reset | LCD1_RST = E1M-X IO13; Linux drives it via `gpio-gd32-bridge` on V2N-family SoMs. |
| Panel power | LCD1_PWR_EN = E1M-X IO15 — pulled high on the carrier, so the panel powers by default without explicit firmware action. A SoM's route to the pad (the V2N family's bridge bit and minimum protocol minor) is in its `pad_routes` and `docs/gd32-bridge-protocol.md`. |
| Touch controller | Goodix GT911 on the J6 display I2C, which is E1M-X I2C3 (pads A23/A24, through a level shifter), the same bus as CAM1.  On V2N-family SoMs E1M-X I2C3 reaches only the GD32 (`PC8`/`PC9`) and is a Linux I2C adapter served by the GD32 bridge I2C proxy ([`../gd32-bridge-protocol.md`](../gd32-bridge-protocol.md) section 3.20). |
| Silicon note | Datasheet R01DS0466 rev 1.20 section `#AC0`/`#BC0` states those part suffixes do not support MIPI-DSI Display Command Set (DCS) control — HX8394 init (which uses DCS commands) is impossible on `#AC0` parts. The SoM is moving to a later-suffix DCS-capable part; older `#AC0` boards will fail at panel init by design. |
| Bring-up status | Code complete on `feat/v2n-lcd-display1` (kernel patches 0004–0006, DT nodes, weston image, LVGL example); **HIL on silicon pending** (bench ladder G0–G8). |

### Display 2 (J28) — carrier-ready, unavailable on V2N/V2M

The carrier fully wires the DSI1 lane set to J28 for future dual-DSI
SoMs.  V2N-family SoMs (`E1M-V2N101/102`, `E1M-V2M101/102`) have only
one MIPI-DSI output — the DSI1 pads are unpopulated on the SoM.
Display 2, its LCD2_RST (E1M-X IO21), LCD2_PWR_EN (E1M-X IO22), and CTP2 sideband
(IO17/IO19) are therefore **permanently unavailable on V2N/V2M** at this
hardware revision.

## Audio (TAS2563 playback card, V2N Linux)

Linux plays through the ALSA card `e1m-x-evk-tas2563` (`e1m-x-evk.dtsi` in
`meta-alp-sdk`): SSI2 carries the data (P47), with SCK/WS taken from SSI1
(P44/P45) via the `alp,shared-pin-ssi1` property added by kernel patch 0014.
P46 (SSI1 SDATA, the amps' SDOUT net) is deliberately left unmuxed so the SoC
never drives it; there is no capture or IV-sense path.  Bench listen is still
pending (#2331).

## I²C buses and devices

Every I²C bus the carrier exposes with an E1M-V2N / E1M-V2M SoM fitted, and
every fixed device on it (#2645).  Addresses are 7-bit.  The machine-readable
source is `metadata/boards/e1m-x-evk.yaml` (`i2c_devices:`, `audio.codecs`)
for carrier parts and the SoM preset's `on_module.i2c_devices` for
on-module parts; this section is the reading aid.

"Seen on a real unit" means the address answered a read-probe
(`i2cdetect -y -r`) on an E1M-V2M103 in this carrier.  It does not by
itself prove which part answered; where an ID register was also read, the
row says so.

### Buses

| E1M-X bus | Controller on V2N / V2M | Linux adapter | Speed | What is on it |
|---|---|---|---|---|
| I2C0 (`XEVK_I2C_BUS_SENSORS`) | RZ/V2N RIIC0 | `i2c-0` | 100 kHz | Carrier sensors, power monitors, I/O expanders, audio amplifiers; the SoM identity EEPROM; the PCIe / M.2 branch (below) |
| I2C0, PCIe / M.2 branch | same bus, through a level shifter | `i2c-0` | 100 kHz | PCIe I/O expander, then a 2:1 switch to the M.2 E-key or M.2 M-key slot |
| I2C1 | RZ/V2N RIIC1 | `i2c-1` | 400 kHz | 14-pin expansion header only; no fixed device |
| I2C2 (`XEVK_I2C_BUS_DSI_CSI0`) | RZ/V2N RIIC2 | enabled only by the camera device trees; it has no alias, so read its number from `i2cdetect -l` | 400 kHz | Camera connectors (CAM0 pair and the parallel-camera connector); no fixed device |
| I2C3 (`XEVK_I2C_BUS_DSI_CSI1`) | SoM bridge MCU I2C proxy (no RZ/V2N master) | adapter registered by `gpio-gd32-bridge` (DT label `e1m_x_i2c3`); read its number from `i2cdetect -l` | 100 kHz default (100 or 400) | J6 display I2C (panel bridge, GT911 touch) and CAM1 (J12) connector; no fixed device on the bus scan.  The SoM has no I2C3 pull-ups; the carrier or module provides them |
| I3C | RZ/V2N I3C | none | n/a | 3-pin header, not fitted |
| SoM-internal power / clock bus | RZ/V2N RIIC8 | `i2c-8` | 400 kHz | On-module parts only, see the last table |

The PCIe / M.2 switch is enabled by `XEVK_PIN_PCIE0_I2C_EN` and its
direction is set by line P0 of the PCIe I/O expander.  The enable polarity
recorded for that pin has not been checked on a board (#2645).

### Fixed devices on I2C0 (`i2c-0`)

| Device | Part | Address | Address strap | Interrupt / reset / enable | Seen on a real unit | SDK support |
|---|---|---|---|---|---|---|
| IMU (primary) | ICM-42670-P | `0x69` | AD0 high | INT1, INT2, FSYNC on main I/O expander P4, P5, P6 | Yes (2026-09-26, 2026-10-02) | `chips/icm42670` |
| IMU (alternate) | BMI323 | `0x68` | SDO low | INT1 on E1M-X IO32 (`XEVK_PIN_BMI323_INT1`) | `0x68` answered on 2026-10-02.  No ID read | `chips/bmi323` |
| Barometer | BMP581 | `0x47` | SDO high | INT on main I/O expander P7 | Yes; chip ID `0x50` read | `chips/bmp581` |
| +3V3 rail monitor | INA236A | `0x40` | A0 = GND | Alert pin not connected | Yes; manufacturer ID `0x5449` read | `chips/ina236`, `examples/v2n/v2n-power-monitor` |
| +1V8 rail monitor | INA236A | `0x41` | A0 = supply | Alert pin not connected | Yes; manufacturer ID `0x5449` read | same |
| TAS2563 shared address (the camera-rail monitor at this address is not fitted) | n/a | `0x48` | Fixed by the TAS2563 pair | n/a | `0x48` answers; that is the amplifiers, not a monitor (see the section below) | Linux `tas2562` codec; no power-monitor macro |
| Camera rail monitor (VCAM3) | INA236B | `0x49` | A0 = supply | Alert pin not connected | Yes; manufacturer ID `0x5449` read | same |
| +5V input monitor | INA228 | `0x42` | A1 = GND, A0 = SDA | Alert pin not connected | `0x42` answered on 2026-10-02; nothing on 2026-09-26 (#2343).  No ID read.  On the current EVK revision the device's bus pins are documented as swapped and corrected by a hand rework; it answers only on carriers with that rework, so treat no answer as "part absent", not a fault | `chips/ina228` (read over i2c-dev, as `chips/ina236`; `examples/v2n/v2n-power-monitor`); upstream Zephyr's `ti,ina228` is the path for a Zephyr-mastered bus.  Sense shunt 100 mOhm; two selectable shunt scales: +/-163.84 mV (1.6384 A full scale, 3.125 uA/LSB, the board default `XEVK_INA228_ADCRANGE_5V`) or +/-40.96 mV (0.4096 A, 0.78125 uA/LSB).  Not run on hardware |
| Main I/O expander | TCAL9538 | `0x73` | A1 high, A0 high | Reset and interrupt pins are pulled up on the carrier and do not reach the SoM | Yes | `chips/tcal9538` |
| PCIe I/O expander | TCAL9538 | `0x71` | A1 low, A0 high | Same.  P0 = PCIe / M.2 I²C switch select, P1 = M.2 E-key alert, P2 to P4 = E-key reset / wake / clock request, P5 to P7 = M-key reset / wake / clock request (port map from the design data, not bench-verified) | Yes | `chips/tcal9538` |
| Audio amplifier, left | TAS2563 | `0x4D` | Strap resistor | Shutdown and fault lines are shared by both amplifiers, on the E1M-X I2S1_SCLK and I2S1_SDI pads | Yes; kernel codec bound | Linux `tas2562` codec (`e1m-x-evk.dtsi`); `chips/tas2563` |
| Audio amplifier, right | TAS2563 | `0x4E` | Strap resistor | shared, as above | Yes; kernel codec bound | same |
| SoM identity EEPROM (on the module) | N24S128 | `0x50`, plus `0x58` for its identity page | Fixed | none | Yes; written and read back during provisioning | `chips/eeprom_24c128` |

The carrier has no EEPROM of its own: `XEVK_I2C_ADDR_EEPROM` is the SoM's
part, which shares this bus.

On Linux these parts are driven by the SDK's `chips/` drivers over
`/dev/i2c-0`, not by kernel drivers (#2339).  A kernel binding claims the
address and the SDK driver then fails with `ALP_ERR_BUSY`.  The two TAS2563
amplifiers are the exception: the kernel owns them for ALSA.

### Not on a scannable bus, or not resolved

| Device | Part | Address | Status |
|---|---|---|---|
| Display 1 touch controller | Goodix GT911 (on the panel cable) | `0x5D` or `0x14`, chosen by the controller's reset sequence | Interrupt on E1M-X IO9, reset on IO11.  The J6 display I2C is E1M-X I2C3, so the GT911 is reachable through the bridge I2C proxy once the touch driver binds to it.  **Never seen on a real unit** |
| mikroBUS socket I²C | plug-in | depends on the Click board | Controller not confirmed (#2645).  Never scanned |
| M.2 E-key / M-key slot I²C | plug-in | depends on the card | Behind the PCIe / M.2 switch on I2C0.  Never scanned with a card fitted |
| Camera modules | plug-in | depends on the sensor | On I2C2 (CAM0) or I2C3 (CAM1); see [`../v2n-camera-csi.md`](../v2n-camera-csi.md) |
| USB-PD sink controller | CYPD3177 | n/a | Its I²C port is not connected to any host bus; it runs from its strap resistors |

### On-module devices on the SoM-internal bus (`i2c-8`)

Full detail is in [`../soms/v2n.md`](../soms/v2n.md).  Linux is the only
master of this bus.

| Device | Part | Address | Seen on a real unit | SDK support |
|---|---|---|---|---|
| Main PMIC | ACT88760 | `0x25` and `0x26` | Yes; register reads recorded | `chips/act8760` |
| Secondary PMIC | DA9292 | `0x1E` | Yes; device ID `0xEA` read | `chips/da9292` |
| NPU supply bucks (E1M-V2M only) | TPS628640 | `0x44`, `0x48`, `0x4F` | Yes | `chips/tps628640` |
| Optional LPDDR4X buck | TPS628640 | `0x4D` | **No** (not fitted on the units swept) | `chips/tps628640` |
| Temperature sensor | TMP112 | `0x40` | Yes; temperature read | `chips/tmp112`, `examples/v2n/v2n-temp-sensor` |
| Clock generator | 5L35023B | `0x69` | Yes; programmed by U-Boot at every boot | `chips/clk_5l35023b` |
| RTC | RV-3028-C7 | `0x52` | Yes; kernel `rtc-rv3028` bound (`/dev/rtc0`) | Linux RTC; `chips/rv3028c7` |
| Secure element | OPTIGA Trust M | `0x30` | Intermittent (#2507) | `chips/optiga_trust_m` |
| Bridge MCU (I²C slave) | GD32G553 | `0x70` | Yes; kernel `gpio-gd32-bridge` bound | Linux GPIO expander; `chips/gd32g553` |

### Checking a board

```sh
i2cdetect -y -r 0     # carrier sensor bus + SoM identity EEPROM
i2cdetect -y -r 1     # expansion header (empty unless something is plugged in)
i2cdetect -y -r 8     # SoM-internal bus
```

Expected on `i2c-0`: `40 41 42 47 48 49 4d 4e 50 58 68 69 71 73`
(`4d` and `4e` print as `UU` when the audio codec driver is bound).
Expected on `i2c-8` for an E1M-V2M: `1e 25 26 30 40 44 48 4f 52 69 70`
(`52` and `70` print as `UU`; `30` may be missing, #2507).  An E1M-V2N has
no `44`, `48` or `4f` on `i2c-8`.

## I²C address `0x48` (TAS2563 shared address)

`0x48` on `XEVK_I2C_BUS_SENSORS` belongs to the two TAS2563 amplifiers (their
shared / global-call address, per `metadata/chips/tas2563.yaml`).  The
camera-rail monitor (an INA236B, +VCAM2) that the design strapped to the same
address is not fitted, so nothing else may be assigned `0x48` on this bus and
the SDK carries no macro for it.  `chips/tas2563/tas2563.c`
still refuses to target `0x48` itself.

## MicroSD (SDHI1)

Carrier microSD slot, `mmc@15c10000` in the kernel DT (`&sdhi1`,
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`). The
bench dev image roots from this card (`root=/dev/mmcblk1p2`);
on-module eMMC (SDHI0) is the production boot.

| Signal | Pin | Behaviour |
|---|---|---|
| Card detect `SD1_SD1CD` | `PA1` | Socket switch to GND, pulled up to the switched card rail; active-low (`cd-gpios`) |
| IO voltage `µSD1_V_SEL` | `PA2` | SoM selector: low = 3.3 V, high = 1.8 V (`vqmmc_sdhi1`, `e1m-v2n-som.dtsi`) |
| Card power `SD1_SD1PWEN` | `PA3` | Always-on hog; the card-detect pull-up lives on this rail |
| `SDCARD_RST` | `PA4` | M.2 Wi-Fi SDIO reset through the carrier mux, not a microSD signal; undriven |

The carrier SDIO mux (TMUX1574 `U38`/`U39`) switches SD1 between the
microSD socket and the M.2 E-key Wi-Fi SDIO. On the E1M-X EVK V2 its select
line `MUX_SEL.SDIO` is not a GPIO: `R239` (100 kOhm) pulls it low (microSD)
and header `P6` ties it to +3V3 (M.2). E1M `IO27` is unconnected. The
active-low enable `SD_MUX_EN` is E1M `IO29` (a GD32 pad) with no external
pull-down, so it rests on the TMUX1574's internal pull-downs; the slot works
with a blank GD32.

Fastest mode is **SDR50** (1.8 V, 100 MHz). The SD1 pads use
`renesas,output-impedance = <2>`, one step weaker than the eMMC's `<3>`:
at `<3>` the data phase fails with `error -84` (CRC). SDR104 is not
enabled: reads work there (about 78-85 MB/s, bench 2026-09-24/27), but
host-to-card writes at 208 MHz never complete (`mmc1: Card stuck being
busy!`, #2357). With kernel patch `0010` (bounce buffer for multi-segment
requests) SDR50 writes 512 MiB in 20 s and HS in 29 s (E1M-V2M103, bench
2026-09-27); before it every write was a single 4 KiB request (~2.7 MB/s).

**U-Boot numbering differs from Linux.** In ALP U-Boot (patch
`0008`), `mmc0` = SDHI0 eMMC, `mmc1` = SDHI1 microSD (4-bit, 3.3 V, no
UHS, no card-detect), `mmc2` = SDHI2 Wi-Fi SDIO.

## Pending from the user

- Authoritative pad-by-pad routing (which E1M-X pad maps to which
  feature on the board).
- The open I²C items listed under "Not on a scannable bus, or not
  resolved" above (touch controller bus, mikroBUS I²C, PCIe / M.2 switch
  enable polarity).
- Boot-strap dipswitch positions for V2N vs V2N-M1.

When that lands, this doc becomes the SDK-side cheat sheet for
the E1M-X EVK matching what
[`e1m-evk.md`](e1m-evk.md) does for the smaller EVK.
