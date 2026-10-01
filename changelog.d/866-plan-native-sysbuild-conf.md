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

The per-example `CMakeLists.txt` bridge that used to merge the same fragment
is retired (see the last entry below), so the app image sees it exactly once.

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
twister. The `CMakeLists.txt` bridge it replaces is retired (see the last entry below).

`gen_example_alp_conf.py` now exits non-zero for a requested example directory
that matches no `--core` zephyr-conf case (a typo or a non-zephyr-conf example)
instead of printing `wrote 0`, and the case walker and exclusion table it shares
with `check_zephyr_conf_parity.py` are public (`find_cases`,
`EXCLUDED_WITH_REASON`). `alp_template.py validate` writes the same fragment
into its temp tree before running twister (its copied `testcase.yaml` names
`generated/alp.conf`), and the PR template, `docs/testing.md`,
`docs/local-ci.md`, `docs/cross-platform-setup.md`, the native-sim container
README and the example READMEs that run twister now run the generator first.
With the bridge gone nothing regenerates the fragment at configure time, so
run the generator before every twister / bare `west build` after a
`board.yaml` or `metadata/` change.

### Removed — the example `CMakeLists.txt` `alp_project.py --emit zephyr-conf` configure-time bridge (#866)

Every example `CMakeLists.txt` (95 of them, including the per-core multicore
subdirs) used to shell `alp_project.py --emit zephyr-conf --core <id>` during
CMake configure and append the result to `EXTRA_CONF_FILE`. That put
intermediate Python on every configure and duplicated what the build plan
already does; it is deleted, along with the `find_package(Python3)` and
`ALP_SDK_ROOT` resolution that only fed it (the multicore slices keep both for
their `--emit ipc-contract-h` header step). The per-core Kconfig fragment now
reaches each consumer one way: `tan build` via the plan's `configArtefacts` plus
`-DEXTRA_CONF_FILE` (`-D<image>_EXTRA_CONF_FILE` under `--sysbuild`); twister
and a bare `west build` via `scripts/gen_example_alp_conf.py`'s
`<app dir>/generated/alp.conf`; a human via `alp_project.py --emit zephyr-conf
--core <id> --output <file>` and `-DEXTRA_CONF_FILE` (`docs/board-config-emit.md`,
`docs/tutorials/01-first-build.md`).

Because the `--core <id>` literal in each `CMakeLists.txt` was the pre-generator's
source of truth, discovery moved to `board.yaml`: `gen_example_alp_conf.py` and
`check_zephyr_conf_parity.py` now walk every `examples/**/board.yaml`, take each
enabled Zephyr core whose `app:` resolves to a customer app dir
(`_zephyr_app_dir`), and write/compare that dir's fragment — the same 94 cores
the CMake walk found (`rpmsg-imx93` stays excluded, its `hw_rev` is `tbd`).
`check_zephyr_conf_parity.py` keeps its byte-parity check (generator output vs the
`alp_project.py` CLI emit) and now also fails if a bridge is re-added.
`check_core_cmakelists_mapping.py` drops its baked-literal assertion (nothing is
baked any more) and keeps the no-two-cores-share-an-app-dir one, which now
guards the shared `generated/alp.conf` slot. `alp_template.py` no longer
re-derives a `--core` literal inside scaffolded `CMakeLists.txt` files
(`_substitute_cmake_core`, `_cmake_core_map` are gone); the five scaffold emit
snapshots were regenerated and the template catalog's generated-artefact notes
point at `tan build` / the generator.

A bare `west build` of an SDK example now needs the fragment passed explicitly
(see above); before this change the CMake bridge supplied it silently.
