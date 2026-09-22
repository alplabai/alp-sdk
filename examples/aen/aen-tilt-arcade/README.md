# aen-tilt-arcade -- Riftrunner

An original, tilt-steered arcade game on the RK055HDMIPI4MA0 720x1280
portrait panel, for an E1M-AEN SoM (M55-HE) on the E1M-EVK.

## The game

A small triangular craft (the "skiff") idles near the bottom of the portrait
screen. Tilting the board left/right steers it sideways. Diamond-shaped
hazards ("shards") fall from the top, faster as the score grows; touching one
ends the run. Small round pickups ("motes") also fall; catching one adds to
the score, shown as a growing green bar across the top of the screen. A run
ends on the first shard touch, pauses briefly so the final frame stays
readable, then restarts with the score and every entity reset.

There is no touch input on this panel -- the touch controller is unusable on
this hardware (see the shield overlay's own header) -- so steering is
tilt-only.

## What it shows

- **Portable display API only.** Every pixel goes through
  `<alp/display.h>` (`alp_display_open` / `alp_display_get_caps` /
  `alp_display_blit` / `alp_display_clear`) -- no raw Zephyr
  `<zephyr/drivers/display.h>` calls and no direct writes into the cdc200
  framebuffer's SRAM0 region.
- **Dirty rects, not a full-frame redraw.** A full 720x1280 RGB565 frame is
  1,843,200 bytes -- far too slow to push every tick on an M55 at 160 MHz.
  Every sprite (skiff, shards, motes, the starfield) is erased at its old
  position and redrawn at its new one with a small blit (28x28 at most), and
  a stationary sprite costs zero blits.
- **Tilt steering from the on-module BMI323**, via its natural-name driver
  `<alp/chips/bmi323.h>` over the portable I2C surface
  (`<alp/peripheral.h>`). Bus + address come from
  `metadata/boards/e1m-evk.yaml`'s BMI323 entry
  (`EVK_I2C_BUS_SENSORS` / `EVK_I2C_ADDR_BMI323`), the same macros
  `examples/aen/aen-bmi323-regcheck` uses. A dead zone + a one-pole
  low-pass filter keep steering from jittering.
- **Runs with no IMU.** If the BMI323 is absent or fails to init, the game
  prints one line and falls back to a deterministic self-steering "attract
  mode" instead of hanging or exiting.
- **Measured frame rate**, printed every ~3 s via `k_uptime_get()`.

See `src/main.c`'s file header for the full rationale, including the panel's
known intermittent cold-boot init failure (#2199).

## Hardware needed

- E1M-AEN801 or E1M-AEN803 SoM (Alif Ensemble E8, M55-HE) on the E1M-EVK.
- RK055HDMIPI4MA0 panel on connector J6 (`e1m_evk_rk055hdmipi4ma0` shield).
- The on-module BMI323 IMU (optional -- the game runs in attract mode
  without it).

## Build

```
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-tilt-arcade
west flash
```

The shield is appended by `CMakeLists.txt`, so a plain `west build` needs no
`-DSHIELD`. Console is the RAM console (`ram_console_buf`, read over SWD) --
same bench setup as `aen-dsi-display`.
