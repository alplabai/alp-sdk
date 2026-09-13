### Fixed — SD Host Controller pinctrl on `aen-evk-demo` and `aen-sdcard-readout` was muxed to unconnected pads (#2035)

Both `aen-evk-demo` phase 9 and `aen-sdcard-readout` programmed Alif's SD "D"
pad route (`CLK=P4_1`, `CMD=P4_2`, `D0..D3=P6_0..P6_3`), copied unverified
from `alif-ensemble-pinctrl.h` and never checked against the E1M-AEN module
schematic — the `aen-sdcard-readout` README's own phase-9 note literally said
"confirm vs schematic", and it never was. It was wrong: `P4_2` carries no SD
signal at all — it is `QEC3_Y_A`, a rotary encoder input reaching `U22` pin 5
on the EVK, with no SD pull-up — and the D-route data pads land on on-module
pins: `D0`/`D1` on `P6_0`/`P6_1` = E1M `AA2 PDM_D0` / `AA1 PDM_C0`, and
`D2`/`D3` on `P6_2`/`P6_3` — with `input-enable` set — on-module
`OSPI0_D2`/`OSPI0_D3` (`metadata/e1m_modules/aen/alif-ospi.tsv:4-5`), owned by
the resident BL32. No SD card was ever reachable through this route, which is
why every bench run of these examples failed.

Both overlays now program the "B" route (`CLK=P14_1`, `CMD=P14_0`,
`D0..D3=P13_0..P13_3`), which is how the module is actually wired. Five
independent metadata sources agree — `metadata/pinmux/aen.yaml:49-64`,
`metadata/e1m_modules/aen/from-alif.tsv:36-48` (the vendor's own mapping),
`metadata/e1m_modules/aen/inter-chip.tsv:41-46`, `docs/cc3501e-bridge.md:74` —
and the EVK netlist closes the chain at `U39` pin 12 carrying `E1M_CMD` to `E2
AH11`. `P14_0`, `P14_1` and `P13_0..P13_3` also run through unpopulated
zero-ohm links to the CC3501E SDIO (`docs/cc3501e-bridge.md:71-75`), with no
conflict observed on any board built to date. `input-enable` stays exactly as
before, on `CMD` and `D0..D3`, omitted on `CLK`.

Noted for the record, not fixed here: `DWC_SDHC_CMD_LINE_LVL_UP_Pos` in
`zephyr/drivers/sdhc/sdhc_dwc.h` is misnamed bit 23 (DAT3 per the SD Host
Controller present-state layout; the header's own
`DWC_SDHC_CMD_DATA_LINE_STATUS_Msk 0x01F00000U` corroborates bits 20-24), so
`sdhc_dwc.c`'s 1.8V-switch wait polls the wrong line. That is vendored driver
code and out of scope for this fix.
