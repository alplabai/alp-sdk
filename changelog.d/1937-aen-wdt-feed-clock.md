### Fixed — `aen-wdt-feed` watchdog clock is 160 MHz, not a 100 MHz placeholder (#1937)

The example's `rtss_he_clk` fixed-clock carried a 100 MHz placeholder. The
upstream CMSDK watchdog driver computes its reload as `timeout_ms * freq_khz`,
so every requested timeout ran at 0.625x its length. Measured on E1M-AEN803
serial 2026W36-0001, the watchdog counts at 160009 kHz, which matches the Alif
fork's `ensemble_rtss_he.dtsi` (160000000). Both board overlays now say 160 MHz.
