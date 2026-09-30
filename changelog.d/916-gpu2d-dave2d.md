### Fixed — the Alif D/AVE 2D backend for `<alp/gpu2d.h>` runs on AEN silicon (#916)

`src/backends/gpu2d/alif_dave2d.c` had never been compiled against Alif's
D/AVE 2D driver or run on a board; every earlier "GPU2D PASS" was the software
fallback. Brought up on E1M-AEN803 serial 2026W36-0001 (M55-HE) against the
driver at its existing `vendor-sdks` pin in `west.yml`:

- The E8 devicetree gains the engine (`zephyr/dts/alif/ensemble_e8_peripherals.dtsi:1708`
  ("gpu2d: dave2d@49040000 {")), disabled by default: `0x49040000`, IRQ 332,
  clock gate `PERIPH_CLK_ENA.GPU_CKEN` (bit 8 of `0x4903F00C`) as the new
  `ALIF_GPU_CLK`. A header named after Alif's fork
  (`alif_ensemble_clocks.h`) maps the driver's include onto the pinned tree.
- The backend initialises the driver's D0 heap before opening the device
  (`d2_opendevice()` failed without it), in the global SRAM0 bank: the engine
  is an AXI master and the driver does no address translation, so it cannot
  reach M55-HE DTCM. Surfaces have the same requirement.
- Each operation now waits for its own pixels
  (`src/backends/gpu2d/alif_dave2d.c:270` ("static void _submit_and_wait(d2_device *dev)")):
  `d2_startframe()` starts the previous frame's render buffer, so every result
  had arrived one call late.
- Fills and copies store colour and alpha as given (the default colour blend
  is alpha-weighted, and `d2_setcolor()` carries no alpha), SRC_OVER uses
  straight alpha like the fallback, and blends reset the constant alpha that a
  previous fill left behind.

`<alp/gpu2d.h>` now states that blended channels may differ by 1 between
backends (the engine scales by 1/256). `examples/aen/aen-gpu2d-bench` gains
`dave2d.overlay` + `overlay-dave2d.conf` to run on the engine, keeps its
surfaces in SRAM0, reports which backend ran, and checks SRC_OVER to +-1 per
channel.

Bench: `RESULT PASS: GPU2D D/AVE 2D engine fill/blit/blend
(REPLACE/SRC_OVER/ADDITIVE/MULTIPLY) all match on E8`, 4 of 4 runs. ADDITIVE and
MULTIPLY are still served by the software fallback, and surface cache
maintenance with the D-cache on is not handled yet (the bench runs with it off).
