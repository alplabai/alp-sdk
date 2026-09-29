### Fixed

- V2N-family images no longer show two failed units on every boot (#2301):
  - `rng-tools` / `rng-tools-service` are dropped on `rzv2n-family`. On RZ/V2N, rngd 6.16 finds no usable source (hwrng, rndr and jitter all fail initialisation), so it failed after ~16 s of CPU without crediting anything. The kernel CRNG initialises on its own at ~6.9 s.
  - meta-rz-distro's `v4l2-init` (the Renesas EVK OV5645 CRU setup script) stays installed but no longer starts automatically. It exits 1 when that camera is absent, which is every E1M-X EVK today (#1149). New dynamic-layer bbappend under `meta-alp-sdk/dynamic-layers/meta-rz-distro/`.
