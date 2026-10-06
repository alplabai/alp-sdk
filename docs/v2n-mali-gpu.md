# RZ/V2N Mali-G31 GPU

How the Mali-G31 on the RZ/V2N family reaches an Alp SDK application, what
the SDK consumes from the vendor, and what has not run on silicon.

> **Status: BENCH-UNVERIFIED.** The kernel side (kbase + the genpd clock fix)
> has run; the GPU backend of `<alp/gpu2d.h>`, the image wiring in this page
> and the HIL specs have **not** been baked or run on a board.

## Where each piece comes from

| Layer | Provided by | In alp-sdk |
|---|---|---|
| GPU kernel driver (Mali kbase, `mali_kbase`, `/dev/mali0`) | Renesas AI SDK `meta-rz-features/meta-rz-graphics` `kernel-module-mali_1.3.0` | `meta-alp-sdk/dynamic-layers/meta-rz-graphics/recipes-kernel/kernel-module-mali/` -- a bbappend + patch `0005` (GPU clocks left to the CPG genpd; without it the GPU cannot power and the kernel logs `Enabling unprepared ... -> -ESHUTDOWN` on every compositor repaint). Applies only when the vendor layer is present (dynamic layer). |
| EGL / GLES 3 / gbm userspace (`libEGL`, `libGLESv2`, `egl.pc`, `glesv2.pc`) | the same layer's `mali-library_1.3.0` (`virtual/egl`, `virtual/libgles2`, `virtual/libgbm`) | nothing -- consumed from the sysroot |
| Distro/image switches (`opengles` distro feature, `PREFERRED_PROVIDER_*`, `IMAGE_INSTALL:append:mali-family = " libegl libgles2"`) | the same layer's `include/rz-graphics.inc` / `mali-graphics.inc`, set when the layer is loaded | nothing -- the reference wiring is reused, not copied |
| Compositor | `weston` + `weston-init` (poky / meta-rz-distro) | `packagegroup-alp-display` (`IMAGE_FEATURES += "alp-display"`) |
| Portable 2D API on the GPU | **this change** | `src/backends/gpu2d/yocto_gles.c` (ADR 0017 Tier-1.5), `PACKAGECONFIG[gles]` in `alp-sdk_0.6.bb` |
| Capability fact "this SoC has a GPU" | `metadata/socs/renesas/rzv2n/n44.json` `variants[].optional_features.gpu_mali_g31` (per die; four of the eight RZ/V2N dies are fused without the GPU) | the SoM-level capability resolver (`resolve_capabilities`) derives `gpu2d` from it; the SoC-level `ALP_SOC_GPU2D` / `ALP_CAP_GPU2D` stay 0 for n44 (the M33/Zephyr side has no GPU) |

Vulkan is not supported by the Renesas layer (`DISTRO_FEATURES:remove =
"vulkan"`), OpenCL comes from its `libopencl` (`opencl` feature); neither is
wrapped by an `<alp/*>` API.

## Licence placement

The Mali DDK (kernel kbase source and the userspace libraries) is
Arm/Renesas licence-gated (`LICENSE = "CLOSED"` in the vendor recipes). It is
**never** copied into this public repository. It lives in the Renesas AI SDK
Yocto recipe package, which the private `alp-sdk-internal` mirror already
carries under `vendors/renesas-rzv2n/ai-sdk-bsp-v6.30/`
(`rzv2n_ai-sdk_yocto_recipe_v6.30.tar.gz`, Git LFS). Alp-built images take the
GPU stack from there; a customer takes it from their own Renesas download.
Public recipes only *reference* it: the `gles` PACKAGECONFIG turns on when the
vendor layer's `opengles` distro feature is visible, the same opt-in shape as
`ALP_ENABLE_DRPAI` / `RUHMI_DRPAI_TVM_DIR`.

## The portable API on the GPU

`<alp/gpu2d.h>` is a 2D surface (fill, blit, blend on caller-owned memory). The
Mali-G31 has no 2D block, so `yocto_gles.c` maps the three ops onto standard
EGL 1.4 + OpenGL ES 3.0 calls: upload the clipped source (and, for a blend, the
destination) to textures, draw one quad into an off-screen FBO with the blend
factors of the requested mode, read the rect back over the caller's buffer.
It includes no vendor header, so it builds against any conformant stack.

What the SDK decides for you:

- **Selection.** The backend is a wildcard at priority 50, above the CPU
  `sw_fallback` (0). Known gap, not intended design: the in-tree Yocto build
  does not define `CONFIG_ALP_SOC_RENESAS_RZV2N_N44`, so `ALP_SOC_REF_STR` is
  `"unknown"` there and exact-ref backends (for example `v2n_n44_isp`) are not
  selected by `alp_backend_select()`. A wildcard is the only registration that
  can win until the recipe derives the SoC ref from the machine; that changes
  backend selection for every class on the image, so it is not bundled here.
- **Degradation.** `alp_gpu2d_open()` still succeeds with no GPU or no
  compositor; the handle then runs everything on the CPU. Per op, ARGB8888 is the
  only format sent to the GPU, strides must be a whole number of pixels, rects
  below `ALP_GPU2D_GLES_MIN_PIXELS` (16384, **unmeasured**) go to the CPU, and
  any GL error re-runs the op on the CPU with the destination untouched. The
  backend counts both outcomes: the first GL-error fallback prints one
  `[gpu2d/gles] GL op failed, falling back` line to stderr and close() prints
  `[gpu2d/gles] N op(s) ran on the GPU, M fell back ...`. That close line, not
  the capability flag, shows which engine did the work.
- **How an app can tell.** `alp_gpu2d_capabilities()` carries
  `ALP_INSTANCE_CAP_DMA` only when the GPU context came up. That says a context
  exists, not that an op used it (diagnostics, not control flow).
- **Coexisting with an app's own EGL.** Every op saves and restores the
  calling thread's current EGL display, context, surfaces and bound API, and
  the shared default display is never terminated.

Be honest about performance: pixels live in CPU memory, so every GPU op pays an
upload and a read-back. Expect the CPU to win on small rects; the GPU should
only pay off for large, blend-heavy layers. That crossover has not been
measured. A zero-copy path (dma-buf import of the framebuffer) would change
this and is not attempted here.

Context: a 1x1 pbuffer on `EGL_DEFAULT_DISPLAY`. The Renesas default Mali
userspace variant is `wayland`, which needs a running compositor; with none,
open() degrades to the CPU. (The layer also ships an `fbdev` variant;
selecting it is a vendor-layer decision, `MALI_BACKEND_DEFAULT`.)

## Build

Prerequisites: a V2N BSP v6.30 build tree whose `bblayers.conf` includes the
Renesas `meta-rz-features/meta-rz-graphics` layer (from the private mirror
package, or the customer's Renesas download) plus `meta-alp-sdk`; `DISTRO =
"alp"` (inherits `rz-vlp`).

```sh
# one per machine; e1m-v2m103-a55 and e1m-v2n101-a55 are the same die variant
MACHINE=e1m-v2n101-a55 bitbake alp-image-edge
MACHINE=e1m-v2m103-a55 bitbake alp-image-edge
```

Check that the GPU backend is on, before the long build:

```sh
bitbake -e alp-sdk | grep '^PACKAGECONFIG='        # contains "gles"
bitbake alp-sdk -c configure
grep -E 'GPU2D|egl|glesv2' tmp/work/*/alp-sdk/*/temp/log.do_configure
```

No `WARNING ... GPU backend is NOT compiled in` line means pkg-config found
`egl` and `glesv2`. Opt-out:

| Variable | Effect |
|---|---|
| `ALP_ENABLE_GPU2D_GLES = "0"` | keep the CPU backend only (default `"1"`; still needs the vendor layer) |

## Verify on target (not run yet)

1. `tests/hil/v2m103-x-evk/v2m103-mali-gpu-present.yaml` (and the `v2n101`
   copy): `/dev/mali0`, `mali_kbase`, `libEGL`/`libGLESv2` present.
2. `weston` active (`systemctl is-active weston`), then
   `tests/hil/v2m103-x-evk/v2n-gpu2d-compose.yaml` -- the example prints
   `[gpu2d] engine: GPU context up (EGL/GLES)` when a context exists and
   `[gpu2d] PASS` when the blended pixel matches the documented formula within
   1 per channel. A PASS only proves the pixels; read the stderr totals line
   `[gpu2d/gles] N op(s) ran on the GPU, M fell back` (M must be 0, N > 0) to
   confirm the GPU ran. `engine: CPU fallback` means no context came up.

## Not done / open

- No bench run of any of the above. The GPU-vs-CPU crossover size is a guess.
- GPU ops use a CPU-memory round trip; dma-buf zero-copy is future work.
- Other formats (RGB565, A8, RGBA8888, RGB888) always take the CPU path.
- A plain-CMake **static** `libalp_sdk.a` does not pull `yocto_gles.c` (no
  link anchor references it, like the other Linux-only backends); the Yocto
  recipe builds the shared library, where it is always linked.
