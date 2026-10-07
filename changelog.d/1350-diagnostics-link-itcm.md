### Added — `diagnostics.link: itcm` board.yaml knob for AEN Flow C builds (tan-cli#1350)

`board.schema.json` gains `diagnostics.link` (`auto` | `itcm`, default `auto`).
`itcm` asks the planner to link every buildable slice into the Alif Ensemble
M55-HE ITCM (base `0x0`, global window `0x58000000`) for a RAM-run with
`tan flash --ram`, instead of the MRAM slot0 link the production flows need.
Until now the only way to get that image was to hand-copy
`scripts/bench/aen/aen-flowc-itcm.{conf,overlay}` into the app.

Per ADR-0026 the planner is tan's: `tan build` turns the knob into the ITCM
retarget Kconfig fragment and devicetree overlay (the same content as the
bench `aen-flowc-itcm.*` pair, plus `CONFIG_DCACHE=n` and the 16 KiB RAM
console from `aen-bench-shared.conf`) and layers them after the slice's
`alp.conf`. The knob is HE-only: `tan build` refuses it for an M55-HP slice,
any other core, or a non-Alif-Ensemble SoM (`build.link-itcm-unsupported`),
and for an explicit non-RAM console (`build.link-itcm-console-conflict`).
alp-sdk's own `alp_orchestrate` does not implement it. See
`docs/aen-bench-bringup.md` (Flow C).
