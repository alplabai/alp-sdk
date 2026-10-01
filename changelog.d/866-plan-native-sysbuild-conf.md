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
plan schema is unchanged (only a command arg is added), but tan-cli plans on
its own (its relocated planner still emits no sysbuild per-core arg), so it
needs a lockstep change before the bridge is retired. The prefix is the app
directory's real basename: when the app directory is the project root, a
tokened plan materialised under a differently-named root must re-derive it.

The seam-1 comparator (`tests/parity/seam1_field_diff.py`) treats the
image-scoped arg as an intended delta on sysbuild slices while a bare
`-DEXTRA_CONF_FILE` there still fails. The `iot-fleet-ota` emit snapshot
changed (one new arg per slice); no other snapshot did.

Not yet done in this change: the per-example `CMakeLists.txt` bridge still
runs and merges the same fragment, so the app image sees it twice (harmless,
idempotent) until the bridge is retired in follow-up slices.

### Changed — twister and a bare `west build` read a pre-generated `generated/alp.conf` (#866)

`scripts/gen_example_alp_conf.py` walks every example `CMakeLists.txt` that
emits a `--core`-scoped `zephyr-conf` and writes
`<example>/generated/alp.conf` (git-ignored) from the same
`_slice_alp_conf` the build plan's `configArtefacts` use. Each example's
`testcase.yaml` now passes it first in `EXTRA_CONF_FILE`, joined with any
existing `native_sim.conf` / `overlay-*.conf` entry by `;` (twister's
`extra_args` replaces rather than appends, so a second `-DEXTRA_CONF_FILE`
would drop the first); later overlays still win. `pr-twister.yml`,
`pr-twister-aen.yml` and `scripts/test-all.sh` run the generator before
twister. The `CMakeLists.txt` bridge still runs alongside; it is retired in a
follow-up slice.

`gen_example_alp_conf.py` now exits non-zero for a requested example directory
that matches no `--core` zephyr-conf case (a typo or a non-zephyr-conf example)
instead of printing `wrote 0`, and the case walker and exclusion table it shares
with `check_zephyr_conf_parity.py` are public (`find_cases`,
`EXCLUDED_WITH_REASON`). `alp_template.py validate` writes the same fragment
into its temp tree before running twister (its copied `testcase.yaml` names
`generated/alp.conf`), and the PR template, `docs/testing.md`,
`docs/local-ci.md`, `docs/cross-platform-setup.md`, the native-sim container
README and the example READMEs that run twister now run the generator first.
Until the `CMakeLists.txt` bridge is retired a stale fragment is harmless (the
bridge's fresh build-dir copy wins); the retiring slice must regenerate it
before every build.
