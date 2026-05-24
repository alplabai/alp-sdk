# drone-hud

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Builds on
> `native_sim/native/64`; HiL silicon bring-up of the IMU + GPS +
> battery + display chain lands in v0.6.

Marketing-grade demo: an E1M-AEN module renders a full drone
telemetry HUD on a 240×320 TFT, sourcing live data from four
on-board sensor chips.

## What it shows

- One app drives **four chips** (LSM6DSO IMU, NEO-M9N GNSS,
  INA236 battery monitor, ST7789 TFT) through the portable
  `<alp/*>` surfaces.  Zero vendor-specific symbols in the app
  code.
- The **`madgwick_ahrs`** library fuses raw IMU samples into a
  stable attitude estimate.  Loader-emitted `CONFIG_ALP_MADGWICK_FPU=y`
  on AEN; the `tmu_cordic` CORDIC path fires on V2N once the
  GD32-bridge build lands.
- **LVGL** composes the attitude indicator, GPS readout, battery
  telemetry, and flight mode into a coherent HUD layout.
- **Three threads**: render (main, 30 fps), IMU+AHRS (100 Hz),
  slow telemetry (5 Hz).  No locks needed -- the
  `drone_telemetry_t` struct is single-writer/single-reader per
  field.

## Hardware needed

- E1M-AEN family SoM (E7 recommended for SRAM headroom).
- E1M-EVK board with:
  - LSM6DSO IMU breakout on I²C0 (already on the EVK).
  - uBlox NEO-M9N GNSS on UART0 (or any UART; edit `board.yaml`).
  - INA236 battery monitor on I²C0 (or any I²C bus).
  - ST7789 240×320 TFT on SPI1 + 2 GPIOs.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/drone-hud
west flash
```

Or in the desktop simulator:

```
west build -b native_sim/native/64 examples/drone-hud
build/zephyr/zephyr.exe
```

The HUD renders even when sensors are absent -- missing chips
log a warning and the matching telemetry fields read zero.

## Customisation

The HUD layout lives in `src/hud_ui.c` -- pure LVGL.  Replace
the rectangular "horizon" with a tilted bitmap, add a pitch
ladder, change the colour scheme.  The data path
(`src/sensors.c`) stays portable.

## Showcase pointers

This demo is the v1.0 readiness pitch for the SDK: one Customer
copies this `examples/drone-hud/` to start their own UAV
firmware, swaps the chips list in `board.yaml`, and inherits the
SDK's portability guarantees.
