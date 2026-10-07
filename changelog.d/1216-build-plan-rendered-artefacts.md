### Added — build-plan-v1 slices carry the rendered `alp.overlay` and `cmake-args.txt` (tan-cli#1216)

A zephyr or baremetal slice's `configArtefacts` gains `alp.overlay`, its DTS
overlay, byte-identical to `alp_project.py --emit dts-overlay --core <id>`
(both call `_slice_dts_overlay`), and `cmake-args.txt`, its full `-D` listing,
identical to `--emit cmake-args --core <id>` minus the `# --- core` marker line
(`_slice_cmake_args`). Additive under `schemaVersion` 1, no bump; the primary
artefact stays first. A board with no header under `include/alp/boards/` emits
a `dts-overlay-unavailable` warning and no overlay artefact instead of failing
the plan. Per ADR-0026 §D, `tan` consumes these bytes rather than re-rendering
them.
