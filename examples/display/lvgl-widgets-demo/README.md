# lvgl-widgets-demo

Wraps the upstream **`lv_demo_widgets()`** from LVGL's `demos/`
tree -- the headline showcase of every standard LVGL widget
(buttons, sliders, tabs, charts, meters, lists, animations).

## What it shows

- LVGL renders into the Zephyr display subsystem unchanged.
- Zephyr's MIPI DBI Type C display path handles ST7789 panel init
  from devicetree.
- Customer code (`src/main.c`) stays board-portable: no
  vendor-specific symbols, only the portable LVGL API.

## Hardware needed

- An E1M-AEN-family SoM (E7 recommended for the SRAM headroom).
- E1M-EVK board (or any board exposing SPI1 + two GPIOs).
- 240 x 320 ST7789 TFT panel wired to:
  - SPI1 → display SCLK + MOSI + CS
  - GPIO IO0 → D/C#
  - GPIO IO1 → RESET (optional; SW-reset works too)
  - 3V3 / GND from the board.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-widgets-demo
west flash
```

## Try it on the desktop first

```
west build -b native_sim/native/64 examples/display/lvgl-widgets-demo
build/zephyr/zephyr.exe
```

LVGL's SDL2 backend renders the demo in a desktop window.  Useful
for UI iteration without touching hardware.

## Verification status

`[UNTESTED]` -- the demo compiles and lv_demo_widgets() is the
upstream LVGL reference target.  The underlying MIPI DBI Type C
devicetree path has build-only CI coverage; real-hardware bring-up
on the E1M-EVK lands once the v1.0 HiL sweep runs.
