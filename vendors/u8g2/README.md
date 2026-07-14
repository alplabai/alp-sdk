# vendors/u8g2

Vendored **minimal core subset** of u8g2 --
<https://github.com/olikraus/u8g2>, tag `2.36.5`, new-BSD (2-clause)
licensed.

## Why vendored instead of west-fetched

u8g2 ships no `zephyr/module.yml`, so it cannot ride in via a plain
`west.yml` project pin the way Zephyr-native modules do -- Zephyr's
module loader silently skips it even when its checkout sits on
`ZEPHYR_MODULES` (see `examples/display/u8g2-oled-draw/CMakeLists.txt`,
which used to require a customer-supplied external checkout for
exactly this reason). A `west.yml` pin is still recorded (behind the
disabled `extras-tier1` group, see the repo-root `west.yml`) as the
audit trail / version source-of-truth for customers who want the
*full* upstream tree (~200 real panel drivers + every bundled font);
the `examples/display/u8g2-oled-draw` teaching example itself builds
against the copy here so it needs no external checkout to build in CI.
This matches the `vendors/etl/` / `vendors/doctest/` precedent for
libraries with no Zephyr module glue.

## What's here -- and why only this much

u8g2's `csrc/` has ~120 files; `u8g2_fonts.c` alone bundles every
built-in font (tens of MB of glyph tables) and `u8g2_d_setup.c` wires
~200 real panel drivers -- neither is reachable from a "no real panel"
native_sim example. `csrc/` here carries only the **core draw
pipeline** the example's `u8g2_SetupBuffer()` / drawing calls /
`u8g2_SendBuffer()` path needs, verified by grepping every `#include`
in this file set (only `u8g2.h` and `u8x8.h` -- no other csrc header):

- `u8x8_setup.c`, `u8x8_display.c`, `u8x8_8x8.c` -- the u8x8 HAL layer
  (device struct setup, display dispatch, byte/bit tiling).
- `u8g2_setup.c`, `u8g2_buffer.c` -- u8g2 struct + RAM buffer sizing.
- `u8g2_hvline.c`, `u8g2_ll_hvline.c`, `u8g2_box.c`, `u8g2_intersection.c`
  -- the horizontal/vertical-line draw primitives every higher-level
  shape (box, line, pixel) reduces to, plus clipping.
- `u8g2_font.c`, `u8g2_kerning.c` -- font/glyph rendering (the font
  *table* is NOT here -- the example supplies its own single extracted
  glyph table at `examples/display/u8g2-oled-draw/src/u8g2_font_6x10_tr.c`,
  not `u8g2_fonts.c`).
- `u8g2.h`, `u8x8.h` -- upstream's public headers, unmodified.

No panel driver (`u8x8_d_*.c`), no font table (`u8g2_fonts.c`), no
input/menu/GUI helpers (`u8g2_input_value.c`, `mui*.c`) -- the example
supplies its own RAM/null device (`u8g2_ram_null.c`) instead of a real
panel driver, so none of those 100+ files are reachable or vendored.

## Compile-time configuration

No profile header -- u8g2's defaults (no heap use in the vendored
subset, no exceptions since it's C, `assert.h`/`string.h` only) already
match the SDK's invariants; nothing to override.

## License

`LICENSE` is the upstream new-BSD (2-clause) license text, unmodified.
It also documents the *font* licenses (BDF-derived, X11-style) for the
upstream fonts -- moot here since no font table is vendored; the
example's own extracted glyph table carries its own provenance comment.

## See also

- [`examples/display/u8g2-oled-draw/README.md`](../../examples/display/u8g2-oled-draw/README.md)
  -- the teaching example this vendored copy backs, and the
  `ALP_U8G2_MODULE_DIR` override for building against the full
  upstream checkout instead (real panel drivers, bundled fonts).
- [`vendors/etl/README.md`](../etl/README.md) -- companion precedent
  for a library vendored because it has no Zephyr module glue.
