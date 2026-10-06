### Added — `<alp/gpu2d.h>` runs on the RZ/V2N Mali-G31 through the vendor EGL/GLES stack (#2660)

`src/backends/gpu2d/yocto_gles.c` is a Linux backend that executes `fill_rect`,
`blit` and `blend` on the GPU with standard EGL 1.4 + OpenGL ES 3 calls (ADR
0017 Tier-1.5: glue over the vendor stack, no vendor header or symbol). It
registers as a wildcard at priority 50 above the CPU fallback and hands
anything it cannot or should not do (non-ARGB8888 formats, strides that are not
whole pixels, rects under `ALP_GPU2D_GLES_MIN_PIXELS`, a GL error, or no
GPU/compositor context at all) to the CPU path, so `alp_gpu2d_open()` still
succeeds everywhere. Bench-unverified; the CPU-vs-GPU crossover size is an
unmeasured default. The backend restores the caller's current EGL display/context/surface after every op, never terminates the process-wide default display, and counts GPU ops and GL-error fallbacks (one stderr line on the first fallback, totals at close).

- `src/yocto/CMakeLists.txt`: opt-in `ALP_SDK_USE_GPU2D_GLES` (+ `ALP_SDK_GPU2D_GLES_REQUIRED`), found via pkg-config `egl` + `glesv2`.
- `meta-alp-sdk`: `alp-sdk` gains `PACKAGECONFIG[gles]`, auto-on for a `mali-family` machine when the Renesas meta-rz-graphics layer put `opengles` in `DISTRO_FEATURES` (`ALP_ENABLE_GPU2D_GLES = "0"` opts out). `alp-image-edge` installs the new `alp-gpu2d-compose` demo (it runs on the CPU fallback too).
- `resolve_capabilities` derives the SoM `gpu2d` capability from the die variant's `optional_features.gpu_mali_g31` (one source; the SoC-level `capabilities.gpu2d` is not set for n44 because four of eight dies lack the GPU, so `ALP_SOC_GPU2D` stays 0); the schema descriptions for `gpu2d` / `dave2d` no longer call D/AVE a Renesas block.
- `examples/v2n/v2n-gpu2d-compose` (portable API only) and HIL specs `v2n-gpu2d-compose` + `*-mali-gpu-present` (not bench-run).
- `docs/v2n-mali-gpu.md`: stack map, licence placement, build steps.

The Mali DDK stays in the Renesas layer / private mirror; nothing proprietary is in this repository.
