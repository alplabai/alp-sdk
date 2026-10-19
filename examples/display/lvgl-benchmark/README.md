# lvgl-benchmark

Wraps the upstream **`lv_demo_benchmark()`** -- the canonical
LVGL performance harness.  Walks a fixed scene set (gradients,
alpha blends, text rendering, image rotation, ...) and reports
per-scene FPS at the end.

## Why run it

Compare the same demo across SoMs to see what the
`capabilities:` block in `metadata/e1m_modules/E1M-*.yaml` is
actually buying you at runtime:

- **E1M-AEN801** (E8 silicon) has GPU2D + DAVE2D + DMA2D
  (`metadata/socs/alif/ensemble/e8.json`).  The
  §D.lib.loader emits `CONFIG_ALP_LVGL_GPU2D=y` +
  `CONFIG_ALP_LVGL_DMA2D=y`, and LVGL hardware-accelerates the
  blits.
- **E1M-AEN301** is E3 silicon -- DMA2D only, no GPU2D
  (`metadata/socs/alif/ensemble/e3.json`).  Loader emits
  `CONFIG_ALP_LVGL_DMA2D=y` only; blits fall back to CPU.  FPS
  drop is the visible measure of "what does GPU2D buy me".
- **native_sim/native/64** binds Zephyr's dummy display driver
  through the portable `<alp/display.h>` + `<alp/gui.h>` surfaces
  (no SDL2 window) -- the CPU-only baseline.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

Scene results print to UART after the benchmark finishes (~30 s); read the
per-scene FPS off the Zephyr console.

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/display/lvgl-benchmark/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/display/lvgl-benchmark
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/display/lvgl-benchmark -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Verification status

`[UNTESTED]` -- compiles + the upstream benchmark is well-known
to be stable.  Real per-SoM numbers need a HiL sweep.
