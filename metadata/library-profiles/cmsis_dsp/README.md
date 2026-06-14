# library-profiles/cmsis_dsp

CMSIS-DSP (https://github.com/ARM-software/CMSIS-DSP) is already
opt-in via `CONFIG_CMSIS_DSP=y` in Zephyr and the loader emits
that flag when `cmsis_dsp` appears in `board.yaml`'s `libraries:`
list.

`hw-backends.yaml` pins the per-capability accelerator bindings:
the SIMD axis (Helium MVE / NEON), TMU CORDIC + FFT, and the ADC
stream-DMA path, with scalar libm as the required SW fallback.
The loader resolves each axis against the SoM's `capabilities:`
block and emits the matching `CONFIG_ALP_CMSIS_DSP_*=y`.  CMSIS-DSP's
other compile-time options (target architecture, intrinsic-set
selection) come from the Cortex-M / Cortex-A variant the SoM
ships, which the SDK already knows from
`metadata/e1m_modules/<family>/sku-*.yaml` and emits to the build
as the appropriate `-DARM_MATH_*` define.  Apps that want a
non-default `ARM_MATH_*` configuration set them in their own build
via `target_compile_definitions`.

## See also

- [`../README.md`](../README.md) -- the profile-headers design.
- [`docs/recommended-libraries.md`](../../../docs/recommended-libraries.md)
  -- the Tier-1 list CMSIS-DSP belongs to.
