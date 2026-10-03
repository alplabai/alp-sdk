# v2n-gpu2d-compose

Compose ARGB8888 layers through the portable `<alp/gpu2d.h>` surface and
check the result -- on the RZ/V2N Mali-G31 when the image carries the Renesas
graphics stack, on the CPU otherwise. The source does not change between the
two.

> **`[UNTESTED on silicon]`.** The GPU backend
> ([`src/backends/gpu2d/yocto_gles.c`](../../../src/backends/gpu2d/yocto_gles.c))
> has not run on a board. The CPU path it falls back to is the same
> `sw_fallback` that is unit-tested on native_sim. See
> [`docs/v2n-mali-gpu.md`](../../../docs/v2n-mali-gpu.md).

## What this shows

1. `alp_gpu2d_open()` + the three ops (`fill_rect`, `blit`, `blend`) on
   caller-owned memory -- no EGL, no GLES, no vendor header in the app.
2. How to see which engine served the ops: `alp_gpu2d_capabilities()` carries
   `ALP_INSTANCE_CAP_DMA` for a hardware backend. Diagnostics only; never
   branch drawing code on it.
3. Why the surface is 640x360: the GPU path uploads, draws and reads back, so
   small rects are cheaper on the CPU and the SDK sends them there op by op.

## Build and run

Yocto (recommended): `alp-image-edge` installs the `alp-gpu2d-compose` recipe on
every `mali-family` machine. Standalone: see the comment in
[`CMakeLists.txt`](CMakeLists.txt). Run `v2n-gpu2d-compose` on the target. For
the GPU engine to be selected the image needs the Renesas `meta-rz-graphics`
layer and a running weston (`IMAGE_FEATURES += "alp-display"`) -- see
[`docs/v2n-mali-gpu.md`](../../../docs/v2n-mali-gpu.md).

Expected output:

```
[gpu2d] engine: GPU context up (EGL/GLES)      # or "CPU fallback"
[gpu2d] centre pixel 0x... expected ~0xFF881018 # within 1 per channel of the formula
[gpu2d] 20 x blend 640x360: ... ms total
[gpu2d] PASS
```

The GPU backend also writes `[gpu2d/gles] ...` lines to stderr: one on the
first GL-error fallback, and at close the totals (`N op(s) ran on the GPU, M
fell back`). Those show which engine really ran; `engine:` only shows that a
GPU context came up.

HIL: [`tests/hil/v2m103-x-evk/v2n-gpu2d-compose.yaml`](../../../tests/hil/v2m103-x-evk/v2n-gpu2d-compose.yaml)
(not bench-run).
