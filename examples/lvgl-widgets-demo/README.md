# lvgl-widgets-demo

Wraps the upstream **`lv_demo_widgets()`** from LVGL's `demos/`
tree -- the headline showcase of every standard LVGL widget
(buttons, sliders, tabs, charts, meters, lists, animations).

## What it shows

- LVGL renders into the Zephyr display subsystem unchanged.
- The SDK's `st7789` chip driver handles the panel-specific init.
- Customer code (`src/main.c`) stays carrier-portable: no
  vendor-specific symbols, only `<alp/*>` peripheral surfaces +
  the portable LVGL API.

## Hardware needed

- An E1M-AEN-family SoM (E7 recommended for the SRAM headroom).
- E1M-EVK carrier (or any carrier exposing SPI0 + two GPIOs).
- 240 x 320 ST7789 TFT panel wired to:
  - SPI0 → display SCLK + MOSI + CS
  - GPIO IO0 → D/C#
  - GPIO IO1 → RESET (optional; SW-reset works too)
  - 3V3 / GND from the carrier.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/lvgl-widgets-demo
west flash
```

## Try it on the desktop first

```
west build -b native_sim/native/64 examples/lvgl-widgets-demo
build/zephyr/zephyr.exe
```

LVGL's SDL2 backend renders the demo in a desktop window.  Useful
for UI iteration without touching hardware.

## Verification status

`[UNTESTED]` -- the demo compiles and lv_demo_widgets() is the
upstream LVGL reference target.  Real-hardware bring-up on the
E1M-EVK lands once the v1.0 HiL sweep runs.
