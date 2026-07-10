# u8g2-oled-draw

**[UNTESTED]** -- `native_sim` build-and-run only; not yet run on a
bench SoM.

Draws with [u8g2](https://github.com/olikraus/u8g2), the monochrome-OLED
graphics library behind most small embedded displays (SSD1306, SH1106,
ST7565, ...). No panel is wired up: u8g2 draws into an in-RAM tile
buffer regardless of the target device, so this example points it at a
"null" device (`src/u8g2_ram_null.c`) that reports a 64x32 canvas and
never talks to a bus, then dumps that buffer to the console as ASCII
art -- the same draw calls a real SSD1306 build would issue.

## What this shows

* The u8g2 setup shape every real panel driver in `u8g2_d_setup.c`
  uses (`u8g2_SetupDisplay()` + `u8g2_SetupBuffer()`), applied to a
  hand-written null device instead of a chip driver.
* `u8g2_DrawFrame`, `u8g2_DrawBox`, `u8g2_SetFont` + `u8g2_DrawStr` --
  the core u8g2 drawing primitives.
* Reading the u8g2 RAM buffer directly (there's no "get pixel" call in
  u8g2's own API -- see `u8g2_ram_null_dump()`).
* Why u8g2's core draw pipeline is vendored in-tree instead of
  west-fetched (see "Why vendored" below).

## Why vendored

u8g2 ships **no** `zephyr/module.yml`, so Zephyr's module loader
silently skips it even when its checkout sits on `ZEPHYR_MODULES` --
and its `extras-tier1` west pin (`west.yml`) is behind a
disabled-by-default group, so a standard CI/customer `west update`
never fetches it anyway. u8g2's `csrc/` tree is also not something
you'd want to compile wholesale even if it were fetched: `u8g2_fonts.c`
alone bundles every font u8g2 ships (tens of MB of glyph data) and
`u8g2_d_setup.c` wires ~200 real panel drivers, neither reachable from
a "no real panel" example.

`vendors/u8g2/csrc/` (see that directory's README.md) carries only the
hand-picked core draw pipeline (setup, buffer, hvline, box, font,
kerning, intersection -- plus the u8x8 glue those lean on), compiled
into the build by the alp-sdk Zephyr module (`zephyr/CMakeLists.txt`)
whenever `CONFIG_ALP_SDK_U8G2_VENDORED_CORE` is set (`board.yaml`'s
`libraries: [u8g2]` -- see `scripts/alp_project_emit.py`'s
`_LIBRARY_KCONFIG["u8g2"]`) -- the same auto-wire every other library
gets; this example needs **no** manual CMake beyond its own `src/*.c`
list. (`CONFIG_ALP_U8G2_SW_BLIT` is a separate `default y`
fallback-capability marker, true for every build regardless of this
slice -- it does NOT gate the compiled sources; see
`zephyr/CMakeLists.txt` for why.) This directory supplies its own
null device (`src/u8g2_ram_null.c`) and a single extracted font glyph
table (`src/u8g2_font_6x10_tr.c`, copied verbatim out of
`u8g2_fonts.c` -- see that file's header comment). If a future example
needs a second font or a real panel driver, extend
`vendors/u8g2/csrc/`'s file list (or point at a full external checkout
-- see that README) rather than vendoring the bundled files wholesale.

## Build

```bash
west build -b native_sim/native/64 examples/display/u8g2-oled-draw
west build -t run
```

No `ZEPHYR_MODULES`/`-x` flags needed -- the vendored core builds
standalone. A customer wanting the full upstream tree (real panel
drivers, bundled fonts) instead of the vendored subset can still point
at an external checkout; see `vendors/u8g2/README.md` and this
example's `CMakeLists.txt` comment.

## Expected output

```
[u8g2-oled-draw] 64x32 canvas (# = lit pixel):
................................................................
.##############################################################.
.#............................................................#.
.#............................................................#.
.#..##########..................................................#.
...
.#............................alp..............................#.
.##############################################################.
................................................................
[u8g2-oled-draw] done
```

(Trimmed here for length -- the real dump is 32 lines of 64 characters
each; the string "alp" renders as pixels near the bottom of the
frame, not literal text.)

## HW swap: a real SSD1306 over I2C or SPI

Swap `u8g2_ram_null_Setup()` (src/u8g2_ram_null.c) for one of u8g2's
generated panel setups, e.g. `u8g2_Setup_ssd1306_64x32_1f()` (or the
128x64 variant for a bigger panel) -- same `u8g2_t`, same
`u8g2_DrawFrame`/`u8g2_DrawBox`/`u8g2_DrawStr` calls in `main.c`
afterwards, just backed by a real bus:

```yaml
# board.yaml
cores:
  m55_hp:
    peripherals:
      - i2c        # or spi, depending on the panel's wiring
chips:
  - ssd1306        # chips/ssd1306 -- see its README for the
                    # I2C-vs-SPI pin/config knobs
```

Real panel setups take `byte_cb`/`gpio_and_delay_cb` callbacks that
DO talk to hardware (`u8x8_byte_sw_i2c`/`u8x8_byte_4wire_sw_spi` or an
Arduino/Zephyr-HAL equivalent) -- unlike this example's
`u8x8_dummy_cb`, which intentionally never issues a bus transaction.
`u8g2_InitDisplay()` (commented out in `main.c`) becomes required on
real hardware: it uploads the panel's power-on init sequence.

## Reference

- [u8g2 wiki: setup functions](https://github.com/olikraus/u8g2/wiki/setup_tutorial)
- `chips/ssd1306/` -- this SDK's SSD1306 chip driver.
- `docs/recommended-libraries.md` -- where u8g2 is tracked as a
  Tier-1 third-party library.
