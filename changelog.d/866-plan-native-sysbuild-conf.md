### Changed — a sysbuild build plan now wires each core's `alp.conf` into the application image itself (#866)

A `--sysbuild` Zephyr slice (a `boot:` or `security.psa:` block) used to carry
no per-core config argument at all: a bare `-DEXTRA_CONF_FILE` there lands on
the sysbuild image, not the application, so the per-core `alp.conf` only
reached the app through each example's own `CMakeLists.txt` bridge. The
planner (`scripts/alp_orchestrate/orchestrator.py`, `_slice_command`) now
emits the image-scoped form, `-D<image>_EXTRA_CONF_FILE=<buildDir>/alp.conf`,
where `<image>` is the basename of the application directory handed to
`west build` — the name sysbuild itself gives the application image. The
`alp.conf` is the one already carried in the slice's `configArtefacts`; the
plan schema is unchanged (only a command arg is added), so existing consumers
such as tan-cli are unaffected.

The seam-1 comparator (`tests/parity/seam1_field_diff.py`) treats the
image-scoped arg as an intended delta on sysbuild slices while a bare
`-DEXTRA_CONF_FILE` there still fails. The `iot-fleet-ota` emit snapshot
changed (one new arg per slice); no other snapshot did.

Not yet done in this change: the per-example `CMakeLists.txt` bridge still
runs and merges the same fragment, so the app image sees it twice (harmless,
idempotent) until the bridge is retired in follow-up slices.
