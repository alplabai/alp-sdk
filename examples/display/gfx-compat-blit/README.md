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
  shipping in-tree, and `board.yaml`'s `libraries:` entry (`name: gfx-compat`, scoped to `m55_hp`) just
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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
python3 scripts/gen_example_alp_conf.py examples/display/gfx-compat-blit   # writes generated/alp.conf (#866)
west twister -T examples/display/gfx-compat-blit -p native_sim/native/64

# Equivalent `west build`:
west build -b native_sim/native/64 examples/display/gfx-compat-blit \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
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
generic DMA2D, and SPI-DMA-push backends are declared as
`accelerators:` entries in the `integration.zephyr.hw_backends:` block
of `metadata/libraries/gfx-compat.yaml`, whose `verification:` marks
the library `hil_silicon: untested` / `smoke_tests: build_only` --
see `src/lib/gfx_compat/Kconfig` for the reserved
`CONFIG_ALP_GFX_COMPAT_GPU2D` / `_DMA2D` / `_SPI_DMA` knobs. This
example's `board.yaml` doesn't need to change when those land: the
`libraries:` loader hook picks the highest-priority
backend for the target SoM automatically (same pattern every other
`libraries:` entry in this SDK follows).

## Reference

- `src/lib/gfx_compat/include/gfx_compat.h` -- the full API contract.
- `docs/recommended-libraries.md` -- where gfx_compat is tracked.
