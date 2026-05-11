# library-profiles/cmsis-dsp

CMSIS-DSP (https://github.com/ARM-software/CMSIS-DSP) is already
opt-in via `CONFIG_CMSIS_DSP=y` in Zephyr and the loader emits
that flag when `cmsis_dsp` appears in `alp.yaml`'s `libraries:`
list.

**No profile header needed.**  CMSIS-DSP's compile-time options
(target architecture, intrinsic-set selection) come from the
Cortex-M / Cortex-A variant the SoM ships, which the SDK already
knows from `metadata/e1m_modules/<family>/sku-*.yaml` and emits
to the build as the appropriate `-DARM_MATH_*` define.  Apps
that want a non-default ARM_MATH_* configuration set them in
their own build via target_compile_definitions.

This directory exists so the loader's "profile path" lookup has
a consistent location for every library, even those whose
profile is empty.  Future revisions may land a thin header here
if CMSIS-DSP adds knobs we want pinned (e.g. helium intrinsics
for M55-HP vs M55-HE).

## See also

- [`../README.md`](../README.md) -- the profile-headers design.
- [`docs/recommended-libraries.md`](../../../docs/recommended-libraries.md)
  -- the Tier-1 list CMSIS-DSP belongs to.
