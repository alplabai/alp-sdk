# drone-hud

> ⚠️ **`[UNTESTED]` on hardware.** Builds on
> `native_sim/native/64`; HiL silicon bring-up of the IMU + GPS +
> battery + display chain is still open.

Marketing-grade demo: an E1M-AEN module renders a full drone
telemetry HUD on a 240×320 TFT, sourcing live data from four
on-board sensor chips.

## What it shows

- One app drives **four chips** (LSM6DSO IMU, NEO-M9N GNSS,
  INA236 battery monitor, ST7789 TFT) through the portable
  `<alp/*>` surfaces.  Zero vendor-specific symbols in the app
  code.
- Attitude is **real Madgwick fusion**.  `board.yaml` declares the
  **`madgwick-ahrs`** library and `sensors.c` drives it through
  `<alp/ahrs.h>`: gyro rates and the accelerometer fuse into a
  quaternion, so roll/pitch stay referenced to gravity rather than
  drifting.  `alp_ahrs_update_imu()` owns the filter state and
  `alp_ahrs_euler()` hands back roll/pitch/yaw -- no fusion maths in
  the app itself.
- **LVGL** composes the attitude indicator, GPS readout, battery
  telemetry, and flight mode into a coherent HUD layout, bound to
  the panel via `<alp/display.h>` + `alp_gui_lvgl_attach()`
  (`<alp/gui.h>`) -- no direct `<zephyr/drivers/display.h>` calls
  in app code.
- **Three threads**: render (main, 30 fps), IMU sample (100 Hz),
  slow telemetry (5 Hz).  Each `drone_telemetry_t` field is
  single-writer/single-reader and main renders from a once-per-frame
  copy of the struct.  That copy is a plain (non-atomic) assignment,
  so it can in principle catch a torn field -- harmless for a
  display-only HUD; a control loop would add a mutex/seqlock.

## Hardware needed

On the E1M-EVK (AEN M55-HP targets) the build applies the
`e1m_evk_rk055hdmipi4ma0` shield automatically: a RK055HDMIPI4MA0
720×1280 MIPI-DSI panel (Himax HX8394) on the EVK's J6 connector, the same
panel `examples/connectivity/iot-dashboard` uses. The EVK has no working SPI
route for an ST7789, so the panel described below applies to other boards.
The UI is laid out for 240×320 and is not scaled to the DSI panel; this
pairing is build-checked in CI only, not yet run on the bench. Pass
`-DSHIELD=<other>` to use a different panel.

- E1M-AEN family SoM (E8 recommended for SRAM headroom).
- E1M-EVK board with:
  - LSM6DSO IMU breakout on I²C0 (already on the EVK).
  - uBlox NEO-M9N GNSS on UART0 (or any UART; edit `board.yaml`).
  - INA236 battery monitor on I²C0 (or any I²C bus).
  - ST7789 240×320 TFT on SPI1 + 2 GPIOs.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/display/drone-hud/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/display/drone-hud
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/display/drone-hud -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash

# Or in the desktop simulator:
west build -b native_sim/native/64 examples/display/drone-hud -- -DEXTRA_CONF_FILE=generated/alp.conf
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

This demo is the SDK's portability pitch: one customer
copies this `examples/display/drone-hud/` to start their own UAV
firmware, swaps the chips list in `board.yaml`, and inherits the
SDK's portability guarantees.
