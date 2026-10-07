### Added — `diagnostics.link: itcm` board.yaml knob for AEN Flow C builds (tan-cli#1350)

`board.schema.json` gains `diagnostics.link` (`auto` | `itcm`, default `auto`).
`itcm` asks the planner to link the Alif Ensemble M55-HE slice into the ITCM
(base `0x0`; `0x58000000` is its global alias) for a RAM-run with
`tan flash --ram`, instead of the MRAM slot0 link the production flows need.
Until now the only way to get that image was to hand-copy
`scripts/bench/aen/aen-flowc-itcm.{conf,overlay}` into the app.

Per ADR-0026 the planner is tan's: `tan build` turns the knob into the ITCM
retarget Kconfig fragment and devicetree overlay (the same content as the
bench `aen-flowc-itcm.*` pair, plus `CONFIG_DCACHE=n` and the 16 KiB RAM
console from `aen-bench-shared.conf`) and layers them after the slice's
`alp.conf`. Proven on the E8 M55-HE (E1M-AEN801 / E1M-AEN803) and refused
elsewhere: `tan build` rejects a project with no M55-HE app (an M55-HP-only
project included), any other SKU or a sysbuild project
(`build.link-itcm-unsupported`), and an explicit non-RAM console
(`build.link-itcm-console-conflict`). The slice's manifest entry carries
`flash_method: ram_run_only`, so plain `tan flash` cannot write the 0x0-linked
image to MRAM. alp-sdk's own `alp_orchestrate` refuses the knob with a message
naming `tan build` rather than silently emitting an MRAM-linked image. See
`docs/aen-bench-bringup.md` (Flow C).
