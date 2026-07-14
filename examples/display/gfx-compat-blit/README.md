# gfx-compat-blit

**[UNTESTED]** -- `native_sim` build-and-run only; not yet run on a
bench SoM.

Uses `gfx_compat` (`src/lib/gfx_compat/`), the SDK's in-tree,
maintainer-written RGB565 fill/blit shim, directly: fill two small
buffers with solid colors, blit one into the other, print a checksum.
No panel, no peripherals -- just RAM buffers.

## What this shows

* `gfx_compat` is **not** a fetched third-party library like the rest
  of `examples/display/`'s siblings -- `west.yml` documents it as
  shipping in-tree, and `board.yaml`'s `libraries: [gfx_compat]` just
  flips `CONFIG_ALP_GFX_COMPAT_SW` (see
  `zephyr/Kconfig.alp-libraries`) to compile
  `src/lib/gfx_compat/gfx_compat.c` into the build.
* `gfx_compat_fill()` / `gfx_compat_blit()`'s exact API (see
  `gfx_compat.h`) -- both operate on flat, contiguous `w*h` buffers
  with **no stride parameter**.
* The row-by-row pointer-arithmetic pattern `gfx_compat.h`'s doc
  comment describes for placing a smaller rect inside a larger
  buffer, since `gfx_compat_blit()` itself doesn't understand a
  destination's stride.

## Build

```bash
west twister -T examples/display/gfx-compat-blit -p native_sim/native/64

# Equivalent `west build`:
west build -b native_sim/native/64 examples/display/gfx-compat-blit \
    -- -DZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

```
[gfx-compat-blit] canvas 16x16, patch 4x4 at (6,6)
[gfx-compat-blit] checksum=0x00e8fe00
[gfx-compat-blit] done
```

The checksum is a plain additive sum over all 256 canvas pixels -- not
cryptographic, just a cheap "did the fill + blit land where expected"
signal for a console-only example. It moves if you change any of
`PATCH_X` / `PATCH_Y` / `PATCH_WIDTH` / `PATCH_HEIGHT` in `src/main.c`.

## HW-accelerated backends

`gfx_compat`'s pure-C `CONFIG_ALP_GFX_COMPAT_SW` path (what this
example exercises) is the only backend implemented today. Alif GPU2D,
generic DMA2D, and SPI-DMA-push backends are metadata-tracked in
`metadata/library-profiles/gfx_compat/hw-backends.yaml` as
`status: planned` -- see `src/lib/gfx_compat/Kconfig` for the reserved
`CONFIG_ALP_GFX_COMPAT_GPU2D` / `_DMA2D` / `_SPI_DMA` knobs. This
example's `board.yaml` doesn't need to change when those land: the
`libraries: [gfx_compat]` loader hook picks the highest-priority
backend for the target SoM automatically (same pattern every other
`libraries:` entry in this SDK follows).

## Reference

- `src/lib/gfx_compat/gfx_compat.h` -- the full API contract.
- `docs/recommended-libraries.md` -- where gfx_compat is tracked.
