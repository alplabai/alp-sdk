# lvgl-benchmark

Wraps the upstream **`lv_demo_benchmark()`** -- the canonical
LVGL performance harness.  Walks a fixed scene set (gradients,
alpha blends, text rendering, image rotation, ...) and reports
per-scene FPS at the end.

## Why run it

Compare the same demo across SoMs to see what the
`capabilities:` block in `metadata/e1m_modules/E1M-*.yaml` is
actually buying you at runtime:

- **E1M-AEN701** has GPU2D + DAVE2D + DMA2D populated.  The
  §D.lib.loader emits `CONFIG_ALP_LVGL_GPU2D=y` +
  `CONFIG_ALP_LVGL_DMA2D=y`, and LVGL hardware-accelerates the
  blits.
- **E1M-AEN301** is E3 silicon -- no GPU2D.  Loader emits
  `CONFIG_ALP_LVGL_DMA2D=y` only; blits fall back to CPU.  FPS
  drop is the visible measure of "what does GPU2D buy me".
- **native_sim/native/64** uses LVGL's SDL2 backend -- the CPU-
  only baseline.

## Build + run

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/display/lvgl-benchmark
west flash
# scene results print to UART after the benchmark finishes (~30 s).
```

Read the per-scene FPS off the Zephyr console.

## Verification status

`[UNTESTED]` -- compiles + the upstream benchmark is well-known
to be stable.  Real per-SoM numbers land in the v1.0 HiL sweep.
