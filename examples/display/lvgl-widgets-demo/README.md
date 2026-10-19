# lvgl-widgets-demo

Wraps the upstream **`lv_demo_widgets()`** from LVGL's `demos/`
tree -- the headline showcase of every standard LVGL widget
(buttons, sliders, tabs, charts, meters, lists, animations).

## What it shows

- LVGL renders through the portable `<alp/display.h>` +
  `<alp/gui.h>` surfaces -- `alp_display_open()` opens the panel,
  `alp_gui_lvgl_attach()` binds it to LVGL's `lv_display_t`. No
  direct `<zephyr/drivers/display.h>` calls in app code.
- Zephyr's MIPI DBI Type C display path handles ST7789 panel init
  from devicetree; the panel is exposed to the app only via the
  `alp-display0` alias the zephyr_drv backend resolves.
- Customer code (`src/main.c`) stays board-portable: no
  vendor-specific symbols, only the portable Alp + LVGL APIs.
- `CONFIG_LV_Z_AUTO_INIT=n` (prj.conf) keeps Zephyr's own lvgl
  module from creating a second, competing `lv_display_t` --
  this app owns `lv_init()`, the LVGL tick source
  (`lv_tick_set_cb`), and the attach call itself.

## Hardware needed

On the E1M-EVK (AEN M55-HP targets) the build applies the
`e1m_evk_rk055hdmipi4ma0` shield automatically: a RK055HDMIPI4MA0
720×1280 MIPI-DSI panel (Himax HX8394) on the EVK's J6 connector, the same
panel `examples/connectivity/iot-dashboard` uses. The EVK has no working SPI
route for an ST7789, so the panel described below applies to other boards.
The UI is laid out for 240×320 and is not scaled to the DSI panel; this
pairing is build-checked in CI only, not yet run on the bench. Pass
`-DSHIELD=<other>` to use a different panel.

- An E1M-AEN-family SoM (E8 recommended for the SRAM headroom).
- E1M-EVK board (or any board exposing SPI1 + two GPIOs).
- 240 x 320 ST7789 TFT panel wired to:
  - SPI1 → display SCLK + MOSI + CS
  - GPIO IO0 → D/C#
  - GPIO IO1 → RESET (optional; SW-reset works too)
  - 3V3 / GND from the board.

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
# writes examples/display/lvgl-widgets-demo/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/display/lvgl-widgets-demo
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/display/lvgl-widgets-demo -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash

# Try it on the desktop first:
west build -b native_sim/native/64 examples/display/lvgl-widgets-demo -- -DEXTRA_CONF_FILE=generated/alp.conf
build/zephyr/zephyr.exe
```

## Desktop run

native_sim binds Zephyr's dummy display driver (no SDL2 window --
see `native_sim.conf`), so this proves the LVGL + `<alp/display.h>`
plumbing (open -> attach -> render -> flush) end to end without a
desktop compositor.

## Verification status

`[UNTESTED on real hardware]` -- native_sim proves the migrated
`alp_display_open()` / `alp_gui_lvgl_attach()` path builds, runs,
and flushes real frames (`alp_display_blit()` is reached from
LVGL's render loop). The underlying MIPI DBI Type C devicetree path
on the E1M-EVK still has build-only CI coverage, so real-hardware
bring-up still needs a HiL sweep.
