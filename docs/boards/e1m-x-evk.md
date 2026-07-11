# E1M-X Development Board — SDK reference

Board for the **E1M-X** form factor (45 × 65 mm) — hosts
the Renesas RZ/V2N family (`E1M-V2N101`, `E1M-V2N102`,
`E1M-V2M101`, `E1M-V2M102`) and any future E1M-X conformant SoM.

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

| SoM SKU       | Backing silicon                                          | Module metadata `soc_ref` |
|---------------|----------------------------------------------------------|---------------------------|
| `E1M-V2N101`  | Renesas `R9A09G056N44GBG#AC0`                            | `renesas:rzv2n:n44`       |
| `E1M-V2N102`  | Renesas `R9A09G056N44GBG#AC0` (different memory tier)    | `renesas:rzv2n:n44`       |
| `E1M-V2M101`  | Renesas `R9A09G056N44GBG#AC0` + DEEPX `DX-M1`            | `renesas:rzv2n:n44` + `deepx:dx:m1` |
| `E1M-V2M102`  | Same, alt memory tier                                    | same                      |

## What this means for the SDK

- v0.2 ships first-class support for the E1M-X EVK + V2N101 SoM
  via `tests/zephyr/peripheral/boards/alp_e1m_evk_v2n.overlay`
  (currently parked, not yet committed) and a placeholder header at
  `include/alp/boards/alp_e1m_evk_v2n.h` (lands with v0.2).
- v0.3 extends to V2M101 / V2M102 with DX-M1 detection on PCIe.
- The Zephyr board file for `alp_e1m_evk_v2n` lives in
  `alplabai/alp-zephyr-modules` (TBD), same split as the AEN EVK.

## Display

### Display 1 (J6) — V2N primary display path

| Item | Detail |
|------|--------|
| Connector | J6 — DSI0 lane set (4 data pairs wired to the E1M-X SoM) |
| Panel | Rocktech RK055HDMIPI4MA0 — 5.5″ 720×1280, Himax HX8394-F controller |
| Link config | 2-lane MIPI-DSI, RGB565 (per the NXP / Rocktech reference configuration) |
| Backlight | SoM-side PWM exposed to Linux as a `pwm-backlight` device tree node; 5 kHz PWM. |
| Panel reset | LCD1_RST = E1M-X IO13; Linux drives it via `gpio-gd32-bridge` on V2N-family SoMs. |
| Panel power | LCD1_PWR_EN = E1M-X IO15 — **unavailable on V2N-family SoMs** but pulled high on the carrier, so the panel powers by default without explicit firmware action. |
| Touch controller | Goodix GT911 on DSI1_CSI_I2C = E1M-X I2C3. **Linux has no I2C master to this bus today on V2N-family SoMs**. Touch support is deferred to a bridge I2C-proxy follow-up; a goodix polled-mode patch ships dormant on the branch in the meantime. |
| Silicon note | Datasheet R01DS0466 rev 1.20 section `#AC0`/`#BC0` states those part suffixes do not support MIPI-DSI Display Command Set (DCS) control — HX8394 init (which uses DCS commands) is impossible on `#AC0` parts. The SoM is moving to a later-suffix DCS-capable part; older `#AC0` boards will fail at panel init by design. |
| Bring-up status | Code complete on `feat/v2n-lcd-display1` (kernel patches 0004–0006, DT nodes, weston image, LVGL example); **HIL on silicon pending** (bench ladder G0–G8). |

### Display 2 (J28) — carrier-ready, unavailable on V2N/V2M

The carrier fully wires the DSI1 lane set to J28 for future dual-DSI
SoMs.  V2N-family SoMs (`E1M-V2N101/102`, `E1M-V2M101/102`) have only
one MIPI-DSI output — the DSI1 pads are unpopulated on the SoM.
Display 2, its LCD2_RST (E1M-X IO21), LCD2_PWR_EN (E1M-X IO22), and CTP2 sideband
(IO17/IO19) are therefore **permanently unavailable on V2N/V2M** at this
hardware revision.

## Pending from the user

- Authoritative pad-by-pad routing (which E1M-X pad maps to which
  feature on the board).
- I²C addresses for any on-board sensors / IO expanders / current
  monitors.
- Boot-strap dipswitch positions for V2N vs V2N-M1.

When that lands, this doc becomes the SDK-side cheat sheet for
the E1M-X EVK matching what
[`docs/boards/e1m-evk.md`](e1m-evk.md) does for the smaller EVK.
