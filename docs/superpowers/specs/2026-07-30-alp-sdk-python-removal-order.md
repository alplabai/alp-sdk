# alp-sdk Python removal order

Status: planning document. No Python changed to produce it — every claim below
was checked against the tree at HEAD of branch `design/tan-python-port` of
`<alp-sdk>` and against branch `feat/python-executor-mvp` of `<tan-cli>`
(`crates/**` and `contract/**` read-only).

## 0. What "the rest of alp-sdk's Python" means today

`git ls-files '*.py' | grep -E '^(scripts|tests)/' | wc -l` returns **289
tracked files** under `scripts/` and `tests/`. That count already excludes
the untracked scratch files listed in this task's hard constraints
(`.alp-support/`, `.codex/`, `metadata/libraries/*.yaml`,
`bash.exe.stackdump` — none are Python and none are touched here).

That 289 is a scoped count, not the repo's whole `.py` surface:
`git ls-files '*.py' | wc -l` returns **298**. The other 9 are out of scope
for this document on purpose — customer/firmware-local generators, not the
planner surface it is about — and nobody has yet made that exclusion a
decision on the record, so it is recorded here: `examples/aen/aen-npu-inference/gen_model.py`,
`examples/aen/aen-npu-inference-alif/gen_model.py`,
`examples/aen/aen-npu-inference-alp/gen_model.py`,
`examples/aen/aen-npu-inference-alp-u55/gen_model.py`,
`examples/aen/aen-npu-inference-person-mram/gen_model.py` (five per-example
NPU model generators — plausibly MOVES-TO-TAN alongside `alp_model`, but
untouched by this plan), `examples/v2n/v2n-m1-ros-perception/launch/perception.launch.py`
(a ROS launch file, not planner-adjacent), `firmware/cc3501e/tests/gen_protocol_vectors.py`,
`firmware/gd32-bridge/tests/gen_protocol_vectors.py`,
`firmware/gd32-bridge/tools/gen_ota_metadata.py` (firmware-local test/tooling
scripts, no board.yaml or `--emit` involvement).

Before enumerating them, one fact reframes the whole exercise: **the planner
has already been relocated once**, on the `tan-cli` side, on the very branch
this task's sibling worktree sits on. `tan-cli/CHANGELOG.md`'s `[Unreleased]`
section (lines 6–31) states it plainly: *"alp-sdk's `scripts/alp_orchestrate/`
(20 modules, ~6.2k lines) is now `python/tan/planner/`... This is a MOVE, not
a rewrite."* `tan-cli/python/tan/planner_root.py` (module docstring, lines
1–33) confirms the shape: `alp_orchestrate` → `tan.planner` verbatim, the
`alp_project.py`-owned loader/emit functions (`resolve_capabilities`,
`resolve_memory_map`, `silicon_to_kconfig`, `som_unpopulated_capabilities`,
`iter_schema_errors`, the SKU/board/pad-route resolvers, every `--emit`
target, and the scaffold/template renderer) → `tan/planner/som_metadata.py`,
`tan/planner/slugs.py`, `tan/planner/loader.py`, `tan/planner/project_loader.py`,
`tan/planner/project_emit/`, `tan/planner/zephyr_board.py`, and
`tan/templates/vendored/`. `planner_root.py` also names the **measured**
zero-Python-loaded invariant:
`python/tests/parity/test_planner_emit_parity.py::test_the_in_process_path_loads_none_of_the_sdks_python`
(the function itself is at `:1426`).

So this document is not "design a move" — the move's *destination* already
exists and is tested against a live import-closure assertion. What remains in
alp-sdk's `scripts/`/`tests/` is (a) alp-sdk's own gates over its own repo
content, which have no reason to ever move, (b) the **superseded copies** of
what already moved, kept alive only because other alp-sdk Python still calls
them, and (c) two scripts that CANNOT be deleted outright because a shipped
`tan` binary or Zephyr's own build system spawns them by path — this is the
subject of Hard Facts 2 and 4 below, and it is the sharpest edge in this
whole plan.

## 1. Every `.py` file, one row each

Legends: **W** = word count of "who calls it" trimmed to the callers that
matter for the disposition (not every transitive test); full caller lists
were captured via `git grep` and are available in this session's scratch
data, omitted here for length. Disposition is exactly one of MOVES-TO-TAN /
STAYS-IN-ALP-SDK / DELETE / BLOCKED per the task's definitions.

### 1.1 `scripts/` — top-level and small packages

| Path | What it does | Who calls it | Disposition |
|---|---|---|---|
| `scripts/abi_snapshot.py` | Extracts/freezes the public C ABI (`include/alp/**/*.h`) into a committed snapshot | `pr-abi-snapshot.yml`, `release.yml`, `bump_version.py` | STAYS-IN-ALP-SDK |
| `scripts/alp_lock/__init__.py` | Pure builder/verifier for `alp.lock` (toolchain + dependency inputs) | `check_toolchain_lock.py`, `gen_sbom.py`, (was) `west alp-lock`, `alp lock` | STAYS-IN-ALP-SDK |
| `scripts/alp_mcp/__init__.py` | Package init for the MCP server | `alp_mcp/server.py` | BLOCKED — see below |
| `scripts/alp_mcp/server.py` | stdio MCP server: DATA tools read `metadata/catalog.json`; LIVE tools shell `validate_board_yaml.py` and `python -m alp_orchestrate` (docstring lines 1–15) | An MCP client (Claude Desktop/Code or any MCP-capable IDE agent) | BLOCKED — its LIVE tools shell two modules this plan deletes (`alp_orchestrate`, `validate_board_yaml.py`); `tan-cli/python/tan/commands/` has no `mcp`/server command today, so there is nothing to repoint the LIVE tools at. DATA tools (pure `catalog.json` reads) have no generator coupling and could survive alone, but the file conflates both — needs a split, not a blanket call, before this can resolve to DELETE or STAYS. |
| `scripts/alp_migrate/__init__.py` | Pure board.yaml schema-version migration engine (byte-faithful text transform) | (was) `west alp-migrate`, `alp_cli/init.py` | BLOCKED — `tan-cli/python/tan/commands/` has no `migrate_cmd.py`; once its only two callers (`west_commands/alp_migrate.py`, `alp_cli/init.py`) are deleted per this plan it has zero front door left. Must either become `tan migrate` or be explicitly retired with a documented "board.yaml files never need re-versioning past schema v1" call — neither has happened. |
| `scripts/alp_migrate/migrations/__init__.py` | Empty migration registry (no migration has landed since schema v1 is still the floor) | `alp_migrate/__init__.py` | Follows `alp_migrate` — BLOCKED |
| `scripts/alp_model/__init__.py` | Package init, no logic | everything under `alp_model/` | Follows the package — MOVES-TO-TAN |
| `scripts/alp_model/_gen_fixture.py` | Test-fixture helper for `.alpmodel` package tests | `test_alp_model_package.py` | MOVES-TO-TAN (fixture, follows tests) |
| `scripts/alp_model/adapters/__init__.py` | `CompilerAdapter` base + adapter registry | `alp_model/build.py` | MOVES-TO-TAN |
| `scripts/alp_model/adapters/cpu.py` | CPU (TFLite-Micro) compiler adapter | `alp_model/build.py`, its tests | MOVES-TO-TAN |
| `scripts/alp_model/adapters/deepx.py` | DEEPX DX-M1 (`dxcom`) compiler adapter | `alp_model/build.py`, `test_deepx_yolo_internal.py` | MOVES-TO-TAN |
| `scripts/alp_model/adapters/drpai.py` | Renesas DRP-AI compiler adapter | `alp_model/build.py` | MOVES-TO-TAN |
| `scripts/alp_model/adapters/ethos_u.py` | Arm Ethos-U (`vela`) compiler adapter | `alp_model/build.py`, `test_vela_yolo_internal.py` | MOVES-TO-TAN |
| `scripts/alp_model/build.py` | Build driver: SKU + source model → `.alpmodel` package (docstring line 2) — textbook "hardware fact → artefact" | `alp_cli/model.py`, `pr-alp-build.yml` | MOVES-TO-TAN |
| `scripts/alp_model/manifest.py` | `.alpmodel` manifest read/write | `alp_orchestrate/__init__.py`, `check_build_plan.py`, `check_system_manifest.py`, `check_bootstrap_manifest.py`, `check_toolchain_lock.py` | MOVES-TO-TAN |
| `scripts/alp_model/package.py` | Assembles/opens the `.alpmodel` zip package | `alp_cli/model.py`, tests | MOVES-TO-TAN |
| `scripts/alp_model/targets.py` | Resolves which NPU/backend targets a SoM needs compiled | `alp_model/adapters/drpai.py`, `alp_project_loader.py` | MOVES-TO-TAN |
| `scripts/alp_model/tensorio.py` | Tensor I/O shape/dtype helpers shared by adapters | tests only | MOVES-TO-TAN |
| `scripts/alp_orchestrate/__init__.py` | Package façade; re-exports `load_board_yaml`, `OrchestratorError`, etc. | **Import-coupled (34 files with a real `from`/`import alp_orchestrate` statement — `git grep -lE '^\s*(from\|import)\s+alp_orchestrate' -- '*.py' \| wc -l` = 34):** `alp_cli/doctor.py`, `alp_cli/validate.py`, `alp_orchestrate/__main__.py`, `alp_orchestrate/models.py`, `alp_project.py`, `alp_project_loader.py`, `check_build_plan.py`, `check_system_manifest.py`, `check_zephyr_conf_parity.py`, `gen_portability_matrix.py`, `kconfig/alp_kconfig_dump.py`, `validate_board_yaml.py`, plus 21 `tests/scripts/test_*.py` files. **Separately — spawns `python -m alp_orchestrate` as a subprocess, not an import (a different removal problem: repoint the argv, no import statement to touch):** `alp_mcp/server.py:401` (`_run([sys.executable, "-m", "alp_orchestrate", ...])`). `gen_catalog.py` and `gen_soc_caps.py` are neither: `gen_catalog.py:74` only reads `alp_orchestrate/cli.py`'s source text via `ast.parse` (no import, no subprocess spawn), and `gen_soc_caps.py`'s two `alp_orchestrate` hits (`:333`, `:383`) are comment/docstring prose only, zero real coupling — none of these three files are among the 34. The task's original "111 distinct files" does not reproduce under any scope tried: `git grep -l alp_orchestrate` = 170 repo-wide, 70 across `scripts/`+`tests/` `*.py`, 41 under `scripts/` alone (prose/comment mentions inflate all three past the real caller count) | MOVES-TO-TAN — **already relocated verbatim** to `tan/planner/__init__.py` |
| `scripts/alp_orchestrate/__main__.py` | `python -m alp_orchestrate` CLI entry | none in-repo (subprocess spawn point only) | MOVES-TO-TAN — already relocated, to `<tan-cli>/python/tan/planner_cli.py` (deliberately not `tan/planner/__main__.py`; its own docstring, line 4: "RELOCATED from alp-sdk `scripts/alp_orchestrate/__main__.py`") |
| `scripts/alp_orchestrate/buildplan.py` | `--emit build-plan` (the customer-facing/IDE build-plan contract) | `check_build_plan.py`, `tests/parity/seam1_field_diff.py` | MOVES-TO-TAN → `tan/planner/buildplan.py` |
| `scripts/alp_orchestrate/carveout.py` | Top-down IPC carve-out allocation | `alp_orchestrate/__init__.py` | MOVES-TO-TAN → `tan/planner/carveout.py` |
| `scripts/alp_orchestrate/cli.py` | `--emit` mode dispatch table for `python -m alp_orchestrate` | `alp_cli/emit.py`, `check_emit_registry.py`, `gen_catalog.py`, `west_commands/alp_emit.py` | MOVES-TO-TAN |
| `scripts/alp_orchestrate/headers.py` | `<alp/system_ipc.h>` header emission | `alp_orchestrate/buildplan.py` | MOVES-TO-TAN → `tan/planner/headers.py` |
| `scripts/alp_orchestrate/kconfig.py` | Per-core Kconfig fragment slicing (`_slice_alp_conf`) — 1570 lines, the largest planner module | `check_zephyr_conf_parity.py`, `check_emit_snapshots.py`, `gen_soc_caps.py`, `kconfig/alp_kconfig_dump.py` | MOVES-TO-TAN → `tan/planner/kconfig.py` |
| `scripts/alp_orchestrate/kconfig_symbols.py` | Promptable-Kconfig-symbol projection (`_project_symbols`) | `kconfig/alp_kconfig_dump.py` **(spawned, see Hard Fact 4)** | MOVES-TO-TAN → `tan/planner/kconfig_symbols.py` |
| `scripts/alp_orchestrate/libraries.py` | ADR-0018 curated third-party library resolution | `alp_cli/doctor.py`, `alp_project.py`, `gen_portability_matrix.py` | MOVES-TO-TAN → `tan/planner/libraries.py` |
| `scripts/alp_orchestrate/loader.py` | board.yaml → topology loader, `iter_schema_errors` | `alp_project.py`, `alp_template.py`, `check_som_topology_parity.py` | MOVES-TO-TAN → `tan/planner/loader.py` |
| `scripts/alp_orchestrate/manifest.py` | `system-manifest.yaml` shape helpers | `check_bootstrap_manifest.py`, `check_build_plan.py`, `check_system_manifest.py` | MOVES-TO-TAN → `tan/planner/manifest.py` |
| `scripts/alp_orchestrate/memregion.py` | Memory-region slug helpers | `alp_orchestrate/slugs.py` | MOVES-TO-TAN |
| `scripts/alp_orchestrate/models.py` | Pydantic-ish plan/topology data models | `gen_portability_matrix.py` | MOVES-TO-TAN → `tan/planner/models.py` |
| `scripts/alp_orchestrate/orchestrator.py` | Top-level orchestration: fans per-core slices, drives buildplan | `alp_orchestrate/__init__.py`, `check_board_target_tree_parity.py` | MOVES-TO-TAN → `tan/planner/orchestrator.py` |
| `scripts/alp_orchestrate/partition.py` | Bottom-up storage partition allocation | `alp_orchestrate/__init__.py` | MOVES-TO-TAN → `tan/planner/partition.py` |
| `scripts/alp_orchestrate/paths.py` | Repo-root path derivation (walk-up from `__file__`) | `alp_orchestrate/memregion.py`, `slugs.py` | MOVES-TO-TAN — **superseded by an explicit binding**, not a walk-up (`planner_root.py`'s whole point is that this exact pattern is unsafe once relocated). Not called by `check_local_paths.py`: that gate's only relationship to this file is its own allowlist entry (`scripts/check_local_paths.py:47`, `"this linter documents the pattern in its docstring"`) — a self-reference, not an import or spawn. |
| `scripts/alp_orchestrate/secure.py` | `security.psa:` TF-M/sysbuild resolution | `alp_orchestrate/buildplan.py`, `alp_cli/validate.py` | MOVES-TO-TAN → `tan/planner/secure.py` |
| `scripts/alp_orchestrate/slugs.py` | Peripheral→Kconfig slug resolution (`peripheral_kconfig`) | `alp_orchestrate/kconfig.py`, `gen_soc_caps.py` | MOVES-TO-TAN → `tan/planner/slugs.py` |
| `scripts/alp_orchestrate/topology.py` | Per-core OS/topology derivation from Cortex class | `alp_orchestrate/paths.py`, `validate.py`, `gen_portability_matrix.py` | MOVES-TO-TAN → `tan/planner/topology.py` |
| `scripts/alp_orchestrate/validate.py` | Cross-field board.yaml validation | `alp_orchestrate/secure.py`, `alp_cli/validate.py` | MOVES-TO-TAN → `tan/planner/validate.py` |
| `scripts/alp_project.py` | Single-core `--emit <mode>` CLI + **compat re-export surface** for `alp_project_loader`/`alp_project_emit` (own docstring, lines 70–79) | `cmake/alp.cmake:131` (fallback path), 99 example `board.yaml`s transitively via CMake, `alp_cli/emit.py`, ~30 test files | **BLOCKED** — see Hard Facts 1, 2 and 4. Cannot simply move or delete: it is (a) `cmake/alp.cmake`'s only fallback emitter, (b) tan's own `TAN_GENERATE_EXECUTOR=subprocess` fallback target (`tan-cli/python/tan/planner_root.py`), and (c) the literal SDK-root discovery marker compiled into shipped `tan` binaries (Hard Fact 2). |
| `scripts/alp_project_emit/__init__.py` | Dispatch façade for the six emit submodules | `alp_project.py`, `alp_cli/emit.py`, `check_emit_snapshots.py` | MOVES-TO-TAN → `tan/planner/project_emit/` (confirmed present) |
| `scripts/alp_project_emit/bom_netlist.py` | `--emit carrier-netlist` (BOM/netlist projection) | `alp_project_emit/__init__.py` | MOVES-TO-TAN |
| `scripts/alp_project_emit/dts.py` | `--emit dts-overlay` | `alp_project_emit/__init__.py` | MOVES-TO-TAN |
| `scripts/alp_project_emit/hw_info.py` | `--emit hw-info-h` | `alp_project_emit/__init__.py`, `alp_template.py` | MOVES-TO-TAN |
| `scripts/alp_project_emit/native_sim.py` | `--emit native-sim-overlay` | `alp_project_emit/__init__.py` | MOVES-TO-TAN |
| `scripts/alp_project_emit/west_libs.py` | `--emit west-libraries` | `alp_project_emit/__init__.py` | MOVES-TO-TAN |
| `scripts/alp_project_loader.py` | board.yaml → SKU/pad-route/capability/memory-map resolution (issue #459 split target) | `alp_project.py:80` (re-export), `alp_project_emit/*`, `alp_cli/new_som.py`, `alp_cli/validator.py`, `check_som_topology_parity.py` | MOVES-TO-TAN → `tan/planner/project_loader.py` |
| `scripts/alp_quality.py` | Runs the quality-task registry for a named profile (`pr`, etc.), emits JUnit/SARIF | `west_commands/alp_quality.py`, `test-all.sh` | STAYS-IN-ALP-SDK — this runs alp-sdk's *own* CI gates locally; it is not board.yaml-derived generation |
| `scripts/alp_registries.py` | Shared metadata-registry loader (`PERIPHERAL_KCONFIG_REGISTRY`) | `alp_orchestrate/slugs.py`, `alp_project_emit/__init__.py` | MOVES-TO-TAN — its one real function (`peripheral_kconfig`) is confirmed already folded into `tan/planner/slugs.py` per `planner_root.py`'s own docstring |
| `scripts/alp_template.py` | Deterministic scaffold render/preview/emit (1544 lines) — `cmake/alp.cmake:58-61` names it as the `--emit scaffold` implementation | `cmake/alp.cmake`, `alp_cli/generate.py`, `alp_project.py` | MOVES-TO-TAN — `tan-cli/python/tan/templates/vendored/{diagnostics,edge-ai,iot,minimal,sensor}/` confirms this already shipped |
| `scripts/build_receipt.py` | Reproducible build-receipt schema/writer (SBOM + lock digest, no wall-clock) | `alp_orchestrate/buildplan.py`, `check_build_receipt.py` | MOVES-TO-TAN — it is executor output attached to the build plan |
| `scripts/bump_version.py` | Bumps alp-sdk's own `VERSION`/`CHANGELOG`/doc-copy of the version | `CMakeLists.txt`, CI release flow | STAYS-IN-ALP-SDK — this versions alp-sdk itself, not a customer artefact |
| `scripts/extract_pdf.py` | Datasheet text extractor for chip-metadata authoring | manual, chip-driver-onboarding workflow | STAYS-IN-ALP-SDK |
| `scripts/flash_backends/__init__.py` | Flash-backend registry, originally for the retired `west alp-flash` (docstring: "Wave 5B... Phase 2 wrote `flash_method:`... system-manifest.yaml") | `provision_som.py` (only live caller today — `west alp-flash` itself no longer exists in this tree) | STAYS-IN-ALP-SDK, **trimmed** — survives as `provision_som.py`'s backend registry once the 3 rows below are deleted |
| `scripts/flash_backends/baremetal_cmake_flash.py` | Invokes a CMake project's `flash`/`program` target | (was) `west alp-flash` | DELETE — `tan-cli/python/tan/commands/flash_cmd.py:5` is an explicit "Port of `crates/tan-cli/src/commands/flash/mod.rs`", mirroring `alp_flash._flash_entry` by name; `tan flash` replaces this role outright |
| `scripts/flash_backends/swd_probe.py` | Flashes a Cortex-M target over SWD (GD32G553 bring-up) | `provision_som.py` | STAYS-IN-ALP-SDK — Alp Lab manufacturing/provisioning tooling, not customer build+flash; no tan equivalent needed or found |
| `scripts/flash_backends/xspi_flashwriter.py` | Writes bl2/fip to blank xSPI via Renesas Flash Writer SCIF mode | `provision_som.py` | STAYS-IN-ALP-SDK — same reasoning |
| `scripts/flash_backends/yocto_wic.py` | Flashes a `.wic`/`.wic.gz` image to SD/eMMC | (was) `west alp-flash` | DELETE — same as `baremetal_cmake_flash.py` |
| `scripts/flash_backends/zephyr_west_flash.py` | Invokes `west flash` on a Zephyr slice's build dir | (was) `west alp-flash` | DELETE — same as `baremetal_cmake_flash.py` |
| `scripts/gen_board_header.py` | Generates `include/alp/boards/alp_<board>_routes.h` from `board.yaml` `e1m_routes:` | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK — generates alp-sdk's own checked-in public header, not a per-customer-project artefact |
| `scripts/gen_catalog.py` | Generates `metadata/catalog.json` (the whole-SDK machine-readable map) | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_error_catalog.py` | Generates `metadata/error-catalog.json` from `alp_status_t` | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_pinmux_capability.py` | Generates per-family pin-mux capability tables from pinout TSVs | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_portability_matrix.py` | Regenerates `docs/portability-matrix.md` | `pr-metadata-validate.yml`, `check_example_portability.py` | STAYS-IN-ALP-SDK — its own tests (`test_gen_portability_matrix*.py`) currently import `alp_orchestrate` to build fixtures, which is a separate, fixable coupling (see 1.3) |
| `scripts/gen_sbom.py` | Deterministic CycloneDX SBOM from `alp.lock` | `check_sbom.py` | STAYS-IN-ALP-SDK |
| `scripts/gen_soc_caps.py` | Generates `include/alp/soc_caps.h` from `metadata/socs/**/*.json` | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_status_strings.py` | Generates `src/status_strings.c` from `alp_status_t` | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_support_matrix.py` | Generates `docs/peripheral-support-matrix.md` | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_tier_a_ci_matrix.py` | Derives `pr-tier-a-libraries.yml`'s build matrix from `metadata/registries/tier-a-library-ci.json` | `pr-tier-a-libraries.yml` | STAYS-IN-ALP-SDK |
| `scripts/gen_zephyr_board.py` | `--emit zephyr-board` — generates the per-core Zephyr board tree (`board.yml`, Kconfig, defconfig, pinctrl, `.dts`) | `alp_project.py`, `alp_orchestrate/__init__.py` | MOVES-TO-TAN — `planner_root.py` names `tan/planner/zephyr_board.py` as the destination explicitly |
| `scripts/kconfig/alp_kconfig_dump.py` | Dumps one board's promptable Kconfig symbols as JSON, run **inside** Zephyr's own `EXTRA_KCONFIG_TARGET` CMake hook | Zephyr's `kconfig.cmake` (spawned, not imported) | **BLOCKED** — see Hard Fact 4. Line 48 imports `alp_orchestrate.kconfig_symbols._project_symbols`; that package is being deleted from alp-sdk in this plan, but this file must remain a standalone spawnable script at a fixed path because Zephyr's CMake hands it a literal `${PYTHON_EXECUTABLE} <this>` command line it cannot redirect at `tan`. |
| `scripts/lint_doc_yaml_fragments.py` | Lints board.yaml YAML fragments embedded in Markdown docs against the schema | doc-lint CI step | STAYS-IN-ALP-SDK |
| `scripts/program_eeprom.py` | Generates the 128-byte on-module EEPROM manifest binary | production-test fixture, `provision_som.py` workflow | STAYS-IN-ALP-SDK — manufacturing-line tooling, not part of a customer build |
| `scripts/provision_som.py` | Orchestrates one E1M SoM's provisioning from a Piece-5 release bundle (flash → EEPROM → power-on test → ledger) | manual, production line | STAYS-IN-ALP-SDK |
| `scripts/quality_tasks.py` | Pure reader for `metadata/quality-tasks-v1.json`; feeds `test-all.sh`'s gate list | `test-all.sh`, `alp_quality.py` | STAYS-IN-ALP-SDK |
| `scripts/som_signing.py` | ECDSA-P256 verify + canonicalization for SoM-release provenance signatures | `provision_som.py`, `check_som_bundle.py` | STAYS-IN-ALP-SDK |
| `scripts/sync_e1m_spec.py` | Refreshes the vendored `e1m-spec` pinout snapshot from the upstream repo | manual, pinout-authoring workflow | STAYS-IN-ALP-SDK |
| `scripts/validate_board_yaml.py` | "Compatibility CLI for the shared board.yaml validator" (own docstring, line 3) — calls `alp_cli.validator` then the orchestrator loader | `pr-alp-build.yml:189`, `cross-platform-zephyr.yml:138`, `pr-metadata-validate.yml:235,243,256`, `alp_mcp/server.py`'s LIVE tools, docs, **and the shipped `tan validate` itself** | **BLOCKED — this row previously said DELETE and that was wrong.** `tan-cli crates/tan-cli/src/commands/validate.rs:5,134-145` *spawns this very script* (`<sdk>/scripts/validate_board_yaml.py --input <board>`), and the Python port refuses the same path with "the full (spawn) validator is not ported yet -- run with --offline, or use the SDK's scripts/validate_board_yaml.py directly" (`python/tan/commands/validate_cmd.py:260-265`). So `validate_cmd.py`/`doctor_cmd.py` are NOT its replacement — one delegates to it and the other is a different verb. It also imports `alp_cli` at module scope (`:18-19`), so it is inside Hard Fact 6's break set. Retained for the record: its own docstring says "historical pre-flight command". `grep -rn 'validate_board_yaml.py' .github/workflows/*.yml` returns 8 lines, but 2 are `paths:` filter entries (`pr-metadata-validate.yml:23,59`) and 1 is a comment (`pr-metadata-validate.yml:247`) — the real invocations are five call sites across three workflows (`pr-alp-build.yml:189`, `cross-platform-zephyr.yml:138`, `pr-metadata-validate.yml:235,243,256`, matching the "Who calls it" column) — **five, and five is what this cell has always said.** An independent review reported this cell as claiming "all four workflow call sites" and asked for the numeral to be corrected; that phrasing never appeared here. `grep -nE 'four\|Four\|FOUR'` over this document before the correction returned 12 lines, none of them about `validate_board_yaml.py`'s call sites, and re-running `grep -rn 'validate_board_yaml.py' .github/workflows/*.yml` returns the same 8 lines / 5 real invocations the cell already lists. No numeral changed. Repointing those five at `tan validate` is *not* available while `tan validate` spawns this script: that is circular. They can only move after Phase 0a. |
| `scripts/validate_metadata.py` | Validates every `metadata/socs/**/*.json`, `metadata/e1m_modules/*.yaml`, `metadata/boards/*.yaml` against schema | `pr-metadata-validate.yml` | STAYS-IN-ALP-SDK |
| `scripts/west_commands/__init__.py` | `west` extension-command registration | `scripts/west-commands.yml` | DELETE |
| `scripts/west_commands/_alp_common.py` | Shared SDK-root discovery for the `west alp-*` commands | every file below | DELETE |
| `scripts/west_commands/alp_emit.py` | `west alp-emit` | `scripts/west-commands.yml:28` | DELETE |
| `scripts/west_commands/alp_lock.py` | `west alp-lock` | `scripts/west-commands.yml:12` | DELETE (front door only — `alp_lock/__init__.py` itself stays) |
| `scripts/west_commands/alp_migrate.py` | `west alp-migrate` | `scripts/west-commands.yml:17` | DELETE (front door only — see `alp_migrate` above for what happens to the logic) |
| `scripts/west_commands/alp_quality.py` | `west alp-quality` | `scripts/west-commands.yml:22` | DELETE (front door only — `alp_quality.py` itself stays) |
| `scripts/west_commands/runners/__init__.py` | Zephyr `west flash` runner-plugin registration | `zephyr/module.yml:40` (`runners:` block) | STAYS-IN-ALP-SDK — genuine Zephyr west-runner extension mechanism, unrelated to the `alp`/`alp_orchestrate` front door |
| `scripts/west_commands/runners/alif_flash.py` | `west flash --runner alif_flash` (Alif E1M-AEN SETOOLS/SE-UART) | `zephyr/module.yml:41` (registration) + `west flash`, bench tooling | STAYS-IN-ALP-SDK |
| `scripts/west_commands/runners/rzv2n_mtd_flash.py` | `west flash --runner rzv2n_mtd_flash` (RZ/V2N mtd) | `zephyr/module.yml:42` (registration) + `west flash`, bench tooling | STAYS-IN-ALP-SDK |
| `scripts/ws6c_emit_parity.py` | WS6-c library-resolution rewrite regression harness: captures/diffs `--emit zephyr-conf` golden output | manual (`--capture`/`--check`), historical migration gate | STAYS-IN-ALP-SDK, but currently spawns `alp_project.py`/`alp_orchestrate` — needs the same rewiring as the 7 `check_*.py` in §1.2 before it can run against a `scripts/`-Python-free tree |

### 1.2 `scripts/check_*.py` (43 files, `git ls-files 'scripts/check_*.py' | wc -l`) and `scripts/alp_cli/**` (19 files)

Every `check_*.py` gates alp-sdk's *own* repository content (headers, docs,
examples, metadata, schemas). None of them turn a customer's `board.yaml`
into a firmware artefact, so **all 43 STAY-IN-ALP-SDK** as a category (the
table below lists all 43). Seven
of them additionally `import alp_orchestrate` or spawn
`python -m alp_orchestrate` / `alp_project.py` directly — those seven are
correctly-placed gates whose *current implementation* is coupled to code this
plan deletes, and that coupling is exactly what blocks deleting §1.1's
MOVES-TO-TAN rows (see §2, Phase 1). They are marked "(import-coupled)"
below; that is not a fourth disposition, it is the ordering hazard the rest
of this document is about.

| Path | What it does | Disposition |
|---|---|---|
| `scripts/check_board_schema_version.py` | Fails if a repo board.yaml has an outstanding schema migration | STAYS-IN-ALP-SDK |
| `scripts/check_board_target_tree_parity.py` | `topology.<core>.board:` ↔ Zephyr board-tree presence gate | STAYS-IN-ALP-SDK |
| `scripts/check_bootstrap_manifest.py` | `metadata/bootstrap.json` lockstep gate (1289 lines, the largest check script) | STAYS-IN-ALP-SDK |
| `scripts/check_build_plan.py` | Validates a build plan against the public alp-CLI/vscode contract | STAYS-IN-ALP-SDK (import-coupled: `from alp_orchestrate import (...)` line 79) |
| `scripts/check_build_receipt.py` | Validates the build-receipt-v1 schema | STAYS-IN-ALP-SDK |
| `scripts/check_chip_header_status.py` | Chip-header `@par Driver status:` ↔ metadata truth gate | STAYS-IN-ALP-SDK |
| `scripts/check_chip_manifest_parity.py` | Chip driver ↔ manifest parity gate | STAYS-IN-ALP-SDK |
| `scripts/check_cmake_chip_list_parity.py` | Plain-CMake chip-list parity gate | STAYS-IN-ALP-SDK |
| `scripts/check_cross_platform.py` | Lints for Linux-only idioms in customer-facing surfaces | STAYS-IN-ALP-SDK |
| `scripts/check_diagnostic_schema.py` | Validates the versioned machine-diagnostics contract | STAYS-IN-ALP-SDK **(alp_cli-coupled: spawns `["-m", "alp_cli.main", "validate", …, "--format", "json"]` line 44)** — a `gate: true` registry entry with no tan replacement; see the third correction under §1.2's `alp_cli` block |
| `scripts/check_doc_drift.py` | Doc-drift gate over `README.md`/`docs/**` identifiers | STAYS-IN-ALP-SDK — see Hard Fact 3 (goes vacuously green, not import-coupled) |
| `scripts/check_doxyfile_single_source.py` | Guards against a second Doxyfile-shaped heredoc | STAYS-IN-ALP-SDK |
| `scripts/check_doxygen_coverage.py` | Doxygen-coverage audit over `include/alp/*.h` | STAYS-IN-ALP-SDK |
| `scripts/check_e1m_pinout.py` | Validates the vendored e1m-spec pinout snapshot | STAYS-IN-ALP-SDK |
| `scripts/check_e1m_route_capability.py` | E1M board-route ↔ SoM capability-table gate | STAYS-IN-ALP-SDK |
| `scripts/check_emit_kconfig_contract.py` | CI schema/smoke contract for `--emit kconfig` | STAYS-IN-ALP-SDK (import-coupled: `["-m", "alp_orchestrate", ...]` line 116) |
| `scripts/check_emit_registry.py` | Validates `metadata/emit-registry-v1.json` against the public IDE/tool contract | STAYS-IN-ALP-SDK (import-coupled: spawns `alp_project.py` + `alp_orchestrate/cli.py` by path, lines 38–39) |
| `scripts/check_emit_snapshots.py` | Byte-for-byte `--emit` snapshot regression gate against `tests/fixtures/emit-snapshots/*.snap` | STAYS-IN-ALP-SDK (import-coupled: `ORCH`/`PROJ` subprocess lists, lines 41–42) — **this is the existing byte-parity oracle every MOVES-TO-TAN row above should be checked against** |
| `scripts/check_example_portability.py` | Cross-family portability lint for `examples/*/{board.yaml,testcase.yaml}` | STAYS-IN-ALP-SDK |
| `scripts/check_example_storage_claims.py` | Rejects examples claiming Alp storage coverage while using Zephyr storage APIs | STAYS-IN-ALP-SDK |
| `scripts/check_inference_backend_parity.py` | Inference backend/format ↔ dispatcher canonicalisation gate | STAYS-IN-ALP-SDK |
| `scripts/check_library_registry.py` | Library alias-table coverage gate | STAYS-IN-ALP-SDK |
| `scripts/check_local_paths.py` | No hard-coded home-directory paths gate | STAYS-IN-ALP-SDK |
| `scripts/check_pin_conflicts.py` | SoM peripheral pad-conflict validation | STAYS-IN-ALP-SDK |
| `scripts/check_plain_cmake_link_complete.py` | Plain-CMake link-completeness gate | STAYS-IN-ALP-SDK |
| `scripts/check_public_header_purity.py` | Root public headers stay chip-neutral gate | STAYS-IN-ALP-SDK |
| `scripts/check_public_private.py` | Public/private classifier gate | STAYS-IN-ALP-SDK |
| `scripts/check_quality_registry.py` | `metadata/quality-tasks-v1.json` ↔ real quality gates lockstep | STAYS-IN-ALP-SDK |
| `scripts/check_sbom.py` | Validates `gen_sbom.py`'s CycloneDX output | STAYS-IN-ALP-SDK |
| `scripts/check_som_bundle.py` | Validates SoM-release bundle manifests | STAYS-IN-ALP-SDK |
| `scripts/check_som_topology_parity.py` | `metadata/socs/**/*.json` `cores[]` ↔ topology gate | STAYS-IN-ALP-SDK (import-coupled: `from alp_project_loader import resolve_soc_path` line 43) |
| `scripts/check_stub_issues.py` | Every `*_stub.c` must reference an open tracking issue | STAYS-IN-ALP-SDK |
| `scripts/check_stub_symbol_matrix.py` | Compile/link symbol-matrix gate for the shared stub backend | STAYS-IN-ALP-SDK |
| `scripts/check_sw_fallback_tags.py` | `sw_fallback.c` Cost/Performance tag gate | STAYS-IN-ALP-SDK |
| `scripts/check_system_manifest.py` | Validates a system manifest against the public IDE/tool contract | STAYS-IN-ALP-SDK (import-coupled: `from alp_orchestrate import (...)` line 79) |
| `scripts/check_tan_docs_surface.py` | Fails when alp-sdk's documented `tan` surface no longer exists in a real `tan --help` | STAYS-IN-ALP-SDK — see Hard Fact 3 (regex-matches the literal string, not import-coupled) |
| `scripts/check_template_catalog.py` | Validates `metadata/templates/catalog-v1.json` | STAYS-IN-ALP-SDK |
| `scripts/check_test_coverage.py` | Portable-core public-header test-coverage audit | STAYS-IN-ALP-SDK |
| `scripts/check_toolchain_lock.py` | `metadata/toolchains.json` (Zephyr SDK pin) lockstep gate | STAYS-IN-ALP-SDK |
| `scripts/check_vendor_ext_tags.py` | `include/alp/ext/**/*.h` vendor-ext tag gate | STAYS-IN-ALP-SDK |
| `scripts/check_version_doc_sync.py` | Version-copy-drift gate | STAYS-IN-ALP-SDK |
| `scripts/check_write_text_newline.py` | `write_text()` CRLF-corruption guard | STAYS-IN-ALP-SDK |
| `scripts/check_zephyr_conf_parity.py` | CMakeLists.txt ↔ `alp_orchestrate`'s Kconfig byte-parity gate | STAYS-IN-ALP-SDK (import-coupled: `from alp_orchestrate import ...` + `from alp_orchestrate.kconfig import _slice_alp_conf`, lines 40–41; also spawns `alp_project.py` — the `subprocess.run(` call at line 148) |

`scripts/alp_cli/**` (19 files: `__init__.py`, `__main__.py`, `_workspace.py`,
`diagnostic.py`, `diagnostic_format.py`, `doctor.py`, `emit.py`, `explain.py`,
`faultdecode.py`, `generate.py`, `init.py`, `main.py`, `model.py`,
`monitor.py`, `new_som.py`, `run.py`, `validate.py`, `validator.py`,
`yaml_pos.py`) — **BLOCKED, not DELETE.**

> **This row was wrong in the first draft of this document and the correction
> is the whole point of Hard Facts 5 and 6.** The draft said "**all DELETE**
> … `tan` already covers every verb alp_cli exposes except `model` … and
> `new-som`/`monitor` (no tan equivalent found — flag as a gap, not a
> blocker)". That is false in three separate ways, each of which would have
> had an executor delete a feature the shipped `tan` binary still needs:
>
> 1. **`faultdecode` does NOT map to `debug_config_cmd.py`.** The draft
>    annotated `debug_config_cmd.py` as "[faultdecode's replacement]". It is
>    not: its own docstring line 2 is "``tan debug-config`` -- generate (or
>    preview) a VS Code launch.json entry". tan contains no ARMv8-M fault
>    arithmetic at all — `grep -rn "CFSR\|HardFault\|faultdecode\|HFSR"
>    --include=*.py .` under `tan-cli/python/tan/` returns exactly one hit,
>    `planner/zephyr_board.py:487`, an unrelated prose comment inside a
>    generated-file string.
> 2. **The forwarded verbs are not "gaps", they are live forwards from a
>    shipped binary.** `tan model` / `tan monitor` / `tan new-som` /
>    `tan faultdecode` do not lack an implementation — their implementation
>    *is* `scripts/alp_cli/`. See Hard Fact 5. Deleting them is feature loss
>    from a released `tan`, not a day-one porting gap.
> 3. **`alp_cli` is not only a command surface.** `scripts/alp_cli/validator.py`
>    is the single shared `board.schema.json` implementation and
>    `scripts/alp_orchestrate/loader.py:29` imports it at module scope. See
>    Hard Fact 6.

The verb-by-verb position, which is the useful artefact here. "tan file" cites
the `tan-cli` repo (worktree `python-executor`); Rust paths are the shipped
binary, `python/tan/…` the Python port:

| `alp` verb | `scripts/alp_cli/` file | tan equivalent | Verdict |
|---|---|---|---|
| `doctor` | `doctor.py` (832) | `tan doctor` — `python/tan/commands/doctor_cmd.py`, `crates/tan-cli/src/commands/doctor.rs` | SUPERSEDED |
| `explain` | `explain.py` (106) | `tan explain` — `commands/explain_cmd.py`, `commands/explain.rs` | SUPERSEDED |
| `generate` | `generate.py` (118) | `tan generate` — `commands/generate_cmd.py`, `commands/generate.rs` | SUPERSEDED |
| `init` | `init.py` (108) | `tan init` — `commands/init_cmd.py`, `commands/init/` | SUPERSEDED |
| `run` | `run.py` (86) | `tan build` — `commands/build_cmd.py`, `commands/run/` | SUPERSEDED |
| `emit` | `emit.py` (222) | `tan generate --emit <mode>` — `commands/generate_cmd.py:16`, `python/tan/planner_emit.py:11-27` | **PARTIAL — six modes have no tan or west front door at all (table below)** |
| `validate` | `validate.py` (43) | `tan validate` — but `crates/tan-cli/src/commands/validate.rs:5,134-145` **spawns `<sdk>/scripts/validate_board_yaml.py`**, which imports `alp_cli` at module scope (`scripts/validate_board_yaml.py:18-19`) | **NOT superseded — implemented BY alp_cli** |
| `model` | `model.py` (51) | none — `tan model` → `sdk_cli::run(…,"model",…)`, `crates/tan-cli/src/main.rs:69` | **BLOCKED (live forward)** |
| `monitor` | `monitor.py` (73) | none — `crates/tan-cli/src/main.rs:70` | **BLOCKED (live forward)** |
| `new-som` | `new_som.py` (645) | none — `crates/tan-cli/src/main.rs:71` | **BLOCKED (live forward)** |
| `faultdecode` | `faultdecode.py` (598) | none — `crates/tan-cli/src/main.rs:72` | **BLOCKED (live forward)** |

Line counts from `wc -l scripts/alp_cli/*.py` (`3998 total` across the 19
files). The four forwarded verbs alone are **1367 lines** — `new_som.py` 645 +
`faultdecode.py` 598 + `monitor.py` 73 + `model.py` 51 — of behaviour that
exists nowhere else in either repo.

**The six orphan `--emit` modes.** `docs/cli.md:333-351` tabulates them as
`python -m alp_cli emit`-only and states it outright: "The six … rows have no
`tan` or `west` front door at all."

| Orphan mode | Owner today | Only front door | Consequence of deleting `alp_cli` |
|---|---|---|---|
| `hw-info-h` | `alp_project.py` | `python -m alp_cli emit` | mode becomes unreachable by any CLI |
| `west-libraries` | `alp_project.py` | `python -m alp_cli emit` | mode becomes unreachable by any CLI |
| `composed-route-table` | `alp_project.py` | `python -m alp_cli emit` | mode becomes unreachable by any CLI |
| `scaffold` | `alp_project.py` | `python -m alp_cli emit` (`--template`/`--sku`) | mode becomes unreachable by any CLI; also the only driver of `check_emit_snapshots.py`'s four scaffold cases (see Phase 1) |
| `zephyr-board` | `alp_project.py` | `python -m alp_cli emit` (`--core`, `--output <dir>`) | mode becomes unreachable by any CLI |
| `os-topology` | orchestrator, via `alp_project.py`'s v2 shim | `python -m alp_cli emit` — `python -m alp_orchestrate --emit os-topology` already fails, exit 2, "invalid choice" | mode becomes unreachable by any CLI |

**Unverified, stated as unverified:** the comparison behind that table is of
mode *LISTS* — `scripts/alp_cli/emit.py:33-76` (15 project + 8 orchestrator
modes) against `python/tan/planner_emit.py:17-27` (11 in-process modes) plus
`python/tan/planner_root.py`'s 8. **Emitted bytes were not compared.** Byte
parity for the 14 overlapping modes is therefore an open question, not a
finding; `scripts/check_emit_snapshots.py` is the existing oracle that would
settle it and has not been run for this purpose.

**Third correction — `scripts/check_diagnostic_schema.py` is coupled, and its
row above understates that.** Its table row says only "STAYS-IN-ALP-SDK" with
no coupling note, but `scripts/check_diagnostic_schema.py:44` spawns
`[sys.executable, "-m", "alp_cli.main", "validate", str(fixture), "--format",
"json"]` — it is a `gate: true` entry in `metadata/quality-tasks-v1.json:129-139`
(`"id": "diagnostic-schema"`), one of the 39 that `scripts/test-all.sh:368`
loads (`python scripts/quality_tasks.py --gate-scripts | wc -l` → `39`), and
it passes *today* precisely because `alp_cli` is there:

```
$ PYTHONPATH="$PWD/scripts" python scripts/check_diagnostic_schema.py
OK   metadata/schemas/diagnostic-v1.schema.json  (self-valid, and a real `alp validate --format json` document conforms, 2 diagnostic(s))
$ echo $?
0
```

**tan cannot take this over.** `metadata/schemas/diagnostic-v1.schema.json`
and its SARIF 2.1.0 export (`scripts/alp_cli/diagnostic_format.py`,
`to_machine_json` / `to_sarif`) have no counterpart in either repo, and
`tan validate --format` accepts only `text|json`
(`python/tan/commands/validate_cmd.py:229,245`; `grep -n sarif` on that file
returns nothing). So this gate cannot be repointed — it can only be retired,
which then also requires deleting its registry entry, because
`scripts/check_quality_registry.py` (CI: `pr-metadata-validate.yml:343-344`)
fails on registry↔disk drift in either direction.

Consequently the 11 `alp_cli` test files (`git ls-files
'tests/scripts/test_alp_cli*.py' 'tests/scripts/test_alp_explain.py'
'tests/scripts/test_alp_faultdecode.py' 'tests/scripts/test_diagnostic*.py'
'tests/scripts/test_yaml_pos.py' 'tests/scripts/test_board_yaml_diagnostics.py'
| wc -l` → `11`) are **BLOCKED too**, not DELETE — §1.3's dispositions for
`test_alp_cli_emit.py` and the `test_alp_cli*.py` group inherit this row's
verdict, and three of them (`test_diagnostic.py`, `test_yaml_pos.py`,
`test_board_yaml_diagnostics.py`) test the shared library of Hard Fact 6
rather than the front door at all. Separately,
`tests/scripts/test_library_layer.py:301-322` holds three
`test_doctor_libraries_*` functions (`:301`, `:307`, `:318`) that
`from alp_cli import doctor` inside the test body, under an `# alp doctor`
banner at `:297-299` that goes with them; that file covers surviving code and
is run by `pr-tier-a-libraries.yml:230`, so it must lose those three functions
rather than be deleted.

### 1.3 `tests/`

| Path | What it does | Follows | Disposition |
|---|---|---|---|
| `tests/__init__.py` | Empty package marker | n/a | STAYS-IN-ALP-SDK |
| `tests/bench/baseline_runner.py` | Performance-baseline harness | bench infra | STAYS-IN-ALP-SDK |
| `tests/fixtures/models/gen_tiny_model.py` | Tiny-model fixture generator for `.alpmodel` tests | `alp_model` | MOVES-TO-TAN |
| `tests/fixtures/models/gen_tiny_onnx.py` | Tiny-ONNX fixture generator | `alp_model` | MOVES-TO-TAN |
| `tests/fuzz/python/board_yaml_loader_fuzz.py` | Atheris fuzz harness for `alp_project.py`'s board.yaml loader | now-relocated loader | MOVES-TO-TAN |
| `tests/fuzz/python/som_preset_yaml_fuzz.py` | Atheris fuzz harness for the SoM-preset YAML loader | now-relocated loader | MOVES-TO-TAN |
| `tests/hil/run_smoke.py` | HiL smoke-test runner | bench infra | STAYS-IN-ALP-SDK |
| `tests/parity/seam1_field_diff.py` | Comparator: live alp-sdk build-plan emit vs. the frozen oracle | `alp_orchestrate` | BLOCKED — this is alp-sdk's own half of the `parity-seam1.yml`/`dispatch-tan-parity.yml` two-seam contract (see Hard Fact 3); it cannot be deleted without either replacing or formally retiring that seam, and no replacement exists in-tree today |
| `tests/parity/test_seam1_field_diff.py` | Negative-matrix test for the comparator | `seam1_field_diff.py` | BLOCKED, follows the above |
| `tests/scripts/__init__.py` | Empty package marker | n/a | STAYS-IN-ALP-SDK |
| `tests/scripts/_orchestrate_support.py` | Shared fixtures for `test_orchestrate_*.py` | `alp_orchestrate` tests | MOVES-TO-TAN (fixture) |
| `tests/scripts/_project_support.py` | Shared fixtures for `test_project_*.py` | `alp_project` tests | MOVES-TO-TAN (fixture) |
| `tests/scripts/conftest.py` | Puts `scripts/` on `sys.path` for the whole suite | every `tests/scripts/*.py` | STAYS-IN-ALP-SDK, but shrinks as rows below leave |

`git ls-files 'tests/scripts/test_*.py' | wc -l` returns **126**, not 128 —
that recount is used everywhere below. These 126 files are not repeated
row-by-row with full prose here (each is a 1:1 unit-test file for exactly one
row already classified above or in §1.2) — they inherit the disposition of
what they test, **except** where their own imports diverge from their
target's disposition. The divergent ones are the load-bearing list, verified
by `git grep -l` against the four module-name patterns (`alp_orchestrate`,
`alp_project_emit`, `alp_project_loader`, `alp_registries`, `alp_project\b`)
across `tests/scripts/test_*.py`: **exactly 44 files**, reproduced verbatim
(this is the task's "roughly forty-four" claim, confirmed exact):

```
test_alp_cli_emit.py                    test_orchestrate_buildplan.py
test_alp_mcp.py                         test_orchestrate_consistency.py
test_alp_project_diagnostics.py         test_orchestrate_libraries.py
test_alp_project_scaffold_emit.py       test_orchestrate_loader.py
test_alp_template.py                    test_orchestrate_manifest.py
test_build_plan_schema.py               test_orchestrate_memory.py
test_check_build_plan.py                test_orchestrate_security.py
test_check_doc_drift.py                 test_orchestrate_slices.py
test_check_emit_registry.py             test_orchestrate_storage.py
test_check_system_manifest.py           test_pad_routes_composition.py
test_check_tan_docs_surface.py          test_project_backends.py
test_dispatch_paths_match_seam1.py      test_project_emit_zephyr.py
test_emit_composed_route_table.py       test_project_hwinfo.py
test_emit_inference_mac.py              test_project_libraries.py
test_emit_kconfig.py                    test_project_loader.py
test_emit_kconfig_workspace.py          test_project_overlay.py
test_emit_os_topology.py                test_project_validation.py
test_gen_portability_matrix.py          test_resolve_capabilities.py
test_gen_portability_matrix_libraries.py test_resolve_memory_map.py
test_gen_zephyr_board.py                test_silicon_determined_fields_rejected.py
test_hil_blocks_coverage.py             test_silicon_variant_and_os_inference.py
test_library_layer.py                   test_topology_default_resolution.py
```

Of these 44, dispositions split as follows:

- **MOVES-TO-TAN** (tests a module that already relocated — becomes the
  parity oracle, or is retired once `tan`'s own test suite covers the same
  ground): all `test_orchestrate_*.py` (9: buildplan, consistency, libraries,
  loader, manifest, memory, security, slices, storage — `git ls-files
  'tests/scripts/test_orchestrate_*.py' | wc -l`), all `test_project_*.py` (7),
  `test_pad_routes_composition.py`, `test_resolve_capabilities.py`,
  `test_resolve_memory_map.py`, `test_silicon_determined_fields_rejected.py`,
  `test_silicon_variant_and_os_inference.py`,
  `test_topology_default_resolution.py`, `test_emit_composed_route_table.py`,
  `test_emit_inference_mac.py`, `test_emit_kconfig.py`,
  `test_emit_kconfig_workspace.py`, `test_emit_os_topology.py`,
  `test_library_layer.py`, `test_alp_project_diagnostics.py`,
  `test_alp_project_scaffold_emit.py`, `test_alp_template.py`,
  `test_gen_zephyr_board.py`.
- **STAYS-IN-ALP-SDK, import-coupled** (tests a `check_*.py` that stays but
  currently reaches into `alp_orchestrate`/`alp_project.py` to build its own
  fixtures — needs rewiring alongside its target, not deletion):
  `test_check_build_plan.py`, `test_check_doc_drift.py`,
  `test_check_emit_registry.py`, `test_check_system_manifest.py`,
  `test_check_tan_docs_surface.py`, `test_build_plan_schema.py`,
  `test_hil_blocks_coverage.py`, `test_gen_portability_matrix.py`,
  `test_gen_portability_matrix_libraries.py`.
- **BLOCKED**: `test_dispatch_paths_match_seam1.py` (Hard Fact 3 — see below;
  it is the vacuous-green risk itself, not a coupled bystander).
- **DELETE** (tests part of the deleted `alp_cli` front door):
  `test_alp_cli_emit.py`.
- **BLOCKED** (tests `alp_mcp`, itself BLOCKED): `test_alp_mcp.py`.

For the other 82 `tests/scripts/test_*.py` files (126 total minus the 44
above), the disposition follows the file they test 1:1 with no divergence
found: `test_check_*.py` → STAYS-IN-ALP-SDK (24 files, one per surviving
`check_*.py` not already in the 44 above — `git ls-files
'tests/scripts/test_check_*.py' | wc -l` returns 29, of which
`test_check_build_plan.py`, `test_check_doc_drift.py`,
`test_check_emit_registry.py`, `test_check_system_manifest.py`, and
`test_check_tan_docs_surface.py` are already counted in the 44 list above,
leaving 24; not 1:1 either — three `test_check_example_portability*.py`
files exist for one `check_example_portability.py` gate); `test_alp_cli*.py`,
`test_alp_explain.py`, `test_alp_faultdecode.py`, `test_diagnostic.py`,
`test_diagnostic_format.py`, `test_yaml_pos.py`, `test_board_yaml_diagnostics.py`
→ DELETE (test the deleted `alp_cli` package, 10 files: `git ls-files` over
that glob set returns 11, minus `test_alp_cli_emit.py` already counted in the
44 above); `test_alp_lock.py` → STAYS-IN-ALP-SDK;
`test_alp_quality.py` → STAYS-IN-ALP-SDK; `test_alp_migrate.py`,
`test_schema_version_negotiation.py` → BLOCKED (follow `alp_migrate`);
`test_alp_model_*.py` (6), `test_deepx_yolo_internal.py`,
`test_vela_yolo_internal.py` → MOVES-TO-TAN (follow `alp_model`);
`test_alif_flash_runner.py`, `test_rzv2n_mtd_flash_runner.py` →
STAYS-IN-ALP-SDK (west runners); `test_flash_backends.py` → BLOCKED, the file
itself needs splitting along the same DELETE/STAYS line as
`scripts/flash_backends/*` before it can resolve; `test_xspi_flashwriter.py`
→ STAYS-IN-ALP-SDK; `test_program_eeprom.py`, `test_provision_som.py`,
`test_som_signing.py` → STAYS-IN-ALP-SDK; `test_gen_board_header.py`,
`test_gen_catalog.py`, `test_gen_error_catalog.py`, `test_gen_sbom.py`,
`test_gen_soc_caps_cap_layer.py`, `test_gen_support_matrix.py`,
`test_gen_tier_a_ci_matrix.py` → STAYS-IN-ALP-SDK; the remaining metadata/
schema/backend-scope regression tests unrelated to the planner
(`test_aen_cc3501e_routes.py`, `test_aen_se_backend_scope.py`,
`test_alif_isp_backend_scope.py`, `test_bitbake_paths_cover_recipe_sources.py`,
`test_board_alias_parity.py`, `test_board_models_schema.py`,
`test_board_schema_version.py`, `test_core_id_enum_coverage.py`,
`test_lint_doc_yaml_fragments.py`, `test_optiga_probe_only_contract.py`,
`test_per_sku_capabilities.py`, `test_quality_registry.py`,
`test_rpc_yocto.py`, `test_soc_debug_probe_identity.py`,
`test_soc_npu_pairing.py`, `test_test_all_worktree.py`,
`test_validate_board_yaml_entrypoints.py` [follows `validate_board_yaml.py`
→ DELETE, once repointed at `tan validate`], `test_validate_metadata_physical.py`)
→ STAYS-IN-ALP-SDK. Four more with no divergence from their target, not
covered by a wildcard above: `test_abi_snapshot.py`,
`test_abi_snapshot_freeze_gate.py` → STAYS-IN-ALP-SDK (follow
`abi_snapshot.py`); `test_build_receipt.py` → MOVES-TO-TAN (follows
`build_receipt.py`); `test_hil_run_smoke.py` → STAYS-IN-ALP-SDK (follows
`tests/hil/run_smoke.py`).

## 2. Ordering — the dependency argument

The constraint is exactly what the task states, now verified: `git grep`
across `scripts/alp_cli/`, `scripts/west_commands/`, `scripts/alp_mcp/`,
`scripts/check_*.py`, and `tests/scripts/test_*.py` for the four module-name
patterns that make up the planner/emit surface — `alp_orchestrate` (20 files,
`git ls-files 'scripts/alp_orchestrate/**' | wc -l`), `alp_project_emit` (6,
`git ls-files 'scripts/alp_project_emit/**' | wc -l`), `alp_project_loader.py`
(1), `alp_registries.py` (1) — is **28**, plus `alp_project.py` (1) which
Phase 3 below explicitly excludes and Phase 4 keeps indefinitely (Hard Fact
2). That fifth module is named here as **"28 + `alp_project.py` (BLOCKED)"**,
not folded into a round 29, because Phase 1 and Phase 3 disagree on whether
it is one of the set being unblocked/deleted — it is unblocked by Phase 1
alongside the other 28, but never deleted. The grep itself returns:

- `scripts/alp_cli/`: 3 files with real `import` statements (`doctor.py`,
  `validate.py`, `new_som.py`) + 2 more with subprocess/path-string
  dependency (`_workspace.py`, `emit.py`) — 5 of 19 files genuinely coupled.
  (`validator.py` matched the initial broad grep too, but its three hits are
  all prose — a docstring/comment naming `alp_orchestrate`/`alp_project.py`,
  never an `import` or a spawned command — the same false-positive shape as
  `check_doc_drift.py` in Hard Fact 3; it is excluded from the coupled count.)
- `scripts/west_commands/`: 2 of 6 (`_alp_common.py`, `alp_emit.py`).
- `scripts/alp_mcp/`: 1 of 2 (`server.py`).
- `scripts/check_*.py`: **exactly 7** — `check_build_plan.py` (import),
  `check_emit_kconfig_contract.py` (subprocess), `check_emit_registry.py`
  (subprocess), `check_emit_snapshots.py` (subprocess),
  `check_som_topology_parity.py` (import), `check_system_manifest.py`
  (import), `check_zephyr_conf_parity.py` (import + subprocess). Two files
  that superficially matched the grep — `check_doc_drift.py` and
  `check_tan_docs_surface.py` — turned out to be **prose-only** mentions
  (a docstring line, a regex over Markdown text), not real dependencies; they
  are excluded from the 7 and are instead Hard Fact 3 material.
- `tests/scripts/test_*.py`: **exactly 44** (§1.3, list reproduced verbatim).

So the task's own numbers hold up under verification, with one correction:
the raw `git grep` hit count on `check_*.py` was 9, not 7 — the two
prose-only files do not belong in the blocking set.

**The ordering, therefore, is forced by who still calls whom, not by
preference:**

0. **Phase 0a — port what `tan` currently FORWARDS, before anything else
   (all work is in `tan-cli`; no alp-sdk code change).** Every later phase
   depends on this one, because until it lands `alp_cli` is not a legacy front
   door — it is the shipped `tan`'s implementation (Hard Fact 5) and the
   planner's schema library (Hard Fact 6). Three deliverables:

   - **The four forwarded verbs.** `tan model` / `tan monitor` / `tan new-som`
     / `tan faultdecode` must stop being `python -m alp_cli <sub>` forwards
     (`crates/tan-cli/src/main.rs:69-72`, `commands/sdk_cli.rs:2-6`) and gain
     real implementations — 1367 lines of behaviour that exists nowhere else
     (`new_som.py` 645, `faultdecode.py` 598, `monitor.py` 73, `model.py` 51).
     `model` additionally waits on `alp_model`'s own MOVES-TO-TAN, which is
     unstarted (`grep -rln "alpmodel\|alp_model" --include=*.py .` under
     `tan-cli/python/tan/` returns nothing).
   - **The six orphan `--emit` modes.** `hw-info-h`, `west-libraries`,
     `composed-route-table`, `scaffold`, `zephyr-board`, `os-topology` reach
     no `tan` and no `west` front door (`docs/cli.md:333-351`), so today
     `python -m alp_cli emit <mode>` is the only way to invoke them. Each needs
     a `tan` front door or an explicit, documented retirement.
   - **A diagnostic-v1 / SARIF `--format` on `tan validate`.** Today it accepts
     only `text|json` (`python/tan/commands/validate_cmd.py:229,245`), and
     `metadata/schemas/diagnostic-v1.schema.json` plus its SARIF 2.1.0 export
     (`scripts/alp_cli/diagnostic_format.py`) have no counterpart in either
     repo. Without this, `scripts/check_diagnostic_schema.py` (a `gate: true`
     registry entry) can only be retired, never repointed. Porting
     `tan validate`'s non-`--offline` spawn path
     (`python/tan/commands/validate_cmd.py:260-265` currently refuses it) also
     belongs here, since that spawn is what couples `tan validate` to
     `scripts/validate_board_yaml.py` and thence to `alp_cli`.

   **Ordering consequence, stated plainly: no `scripts/alp_cli/**` deletion and
   no `pyproject.toml` packaging change can happen before Phase 0a lands.** Not
   as a partial deletion either — deleting the superseded verb modules while
   keeping the forwarded four still removes `main.py`'s click group
   (`scripts/alp_cli/main.py:27-37`), which is the only front door the
   forwarded four have. This phase is numbered `0a` rather than `0` so every
   "Phase 1 / 2 / 3 / 4" cross-reference elsewhere in this document keeps
   pointing at the same phase it always did.

1. **Phase 0b — prerequisites outside this repo (no alp-sdk code change).**
   `tan` needs a public, CI-installable distribution path beyond the one
   `curl | sh` step `tan-docs-drift.yml` already uses (Hard Fact 1), and a
   decision on the four BLOCKED rows that have no tan-side destination today:
   `alp_mcp` (no MCP command in `tan/commands/`), `alp_migrate` (no `migrate`
   command — and see the bootstrap coupling folded into Phase 2 below, which
   makes this decision load-bearing for onboarding, not just the `west
   alp-migrate` verb), and the general question of whether `alp_model`'s
   NPU-compile pipeline is in tan's v1 build scope at all (no `model`
   command, no `tan/planner` counterpart found). None of Phases 1–4 below can
   complete for those rows without this decision; everything else can
   proceed independently.

2. **Phase 1 — rewire the 7 import-coupled `check_*.py` (+ `ws6c_emit_parity.py`,
   + `tests/parity/seam1_field_diff.py`) off `alp_orchestrate`/`alp_project.py`
   and onto a `tan` subprocess call.** Retiring the alp-sdk-side half of a
   check entirely, where `tan` already duplicates it, is available for *some*
   of `check_emit_snapshots.py`'s job — `tan-cli/python/tests/parity/
   test_planner_emit_parity.py` covers `build-plan`, `system-manifest`,
   `kconfig`, `zephyr-conf`, `cmake-args`, `os-topology`, `dts-overlay`,
   `hw-info-h`, `west-libraries`, `native-sim-overlay`, `carrier-netlist`,
   and `yocto-conf` — but **not** `--emit scaffold`: `grep -c scaffold
   python/tests/parity/test_planner_emit_parity.py` returns `0`, while
   `check_emit_snapshots.py` carries four scaffold cases (`"scaffold.minimal-v2n101"`
   :106–107, `"scaffold.peripheral-v2n101"` :120–121, `"scaffold.sensor-v2n101"`
   :127–128, `"scaffold.edge-ai-v2n101"` :132–133). Those four cases are the
   only alp-sdk-side oracle for `scripts/alp_template.py`, which Phase 3
   deletes — so `check_emit_snapshots.py` cannot be fully retired in this
   phase, only rewired for the modes tan already covers; its scaffold cases
   keep running (and keep spawning `alp_template.py`/`alp_project.py`) until
   tan builds a scaffold-mode oracle (§4 makes the same point independently
   and is not in tension with this phase, contrary to an earlier draft of
   this document). This phase is what actually unblocks §1.1's 28 (+
   `alp_project.py`, BLOCKED) MOVES-TO-TAN rows — nothing else does, because
   these are the checks that `import` them, not merely invoke them by path.
   This phase also needs Phase 0b's CI-installable `tan` (a gate cannot
   subprocess to a binary CI doesn't have).

3. **Phase 2 — delete the front doors**: `scripts/alp_cli/**` (19 — **gated on
   Phase 0a; BLOCKED until it lands, see §1.2's `alp_cli` block and Hard Facts
   5 and 6.** Within it, `validator.py`/`diagnostic.py`/`yaml_pos.py` are a
   library the planner still imports and cannot go in the same commit as the
   verb modules),
   `scripts/west_commands/{__init__,_alp_common,alp_emit,alp_lock,alp_migrate,
   alp_quality}.py` (6 — leaving the 3 `runners/` files), `scripts/alp_mcp/**`
   (2, pending Phase 0b's MCP decision), `scripts/flash_backends/
   {baremetal_cmake_flash,yocto_wic,zephyr_west_flash}.py` (3, leaving
   `__init__.py`/`swd_probe.py`/`xspi_flashwriter.py`), and
   `scripts/validate_board_yaml.py` (1 — **also gated on Phase 0a: the shipped
   `tan validate` spawns this script**, so its callers cannot be repointed at
   `tan validate` while that is true. The full CI inventory,
   `grep -rn 'validate_board_yaml.py' .github/workflows/*.yml` → 8 lines, 5 of
   them real invocations: `pr-alp-build.yml:189`,
   `cross-platform-zephyr.yml:138`, and `pr-metadata-validate.yml:235,243,256`
   run it directly; `pr-metadata-validate.yml:23,59` are `paths:` filters and
   `:247` is a comment), plus their ~40
   corresponding `tests/scripts/test_*.py`. Four same-commit edits belong in
   this phase, none of them optional:
   - **`scripts/west-commands.yml`** — the actual west-extension-command
     registration file (see the corrected caller column in §1.1's
     `west_commands/*` rows; `west.yml` only points at it via its `self:`
     block's `west-commands: scripts/west-commands.yml`). It names
     `scripts/west_commands/{alp_lock,alp_migrate,alp_quality,alp_emit}.py`
     by path (`scripts/west-commands.yml:12,17,22,28`) — delete those four
     `.py` files without editing this YAML and west's extension-command
     manifest points at four missing files for every consumer, not just this
     checkout.
   - **`metadata/bootstrap.json:43`**'s `"west": {"extensionGuardCommand":
     "alp-migrate"}` — REQUIRED by `metadata/schemas/bootstrap-v1.schema.json:81`
     — is what `scripts/bootstrap.sh:501` and `scripts/bootstrap.ps1:355`
     both `die`/`Fail` against ("workspace ... does not register 'west
     alp-migrate' ... (#769)") if it doesn't resolve. Deleting
     `scripts/west_commands/alp_migrate.py` without picking a surviving
     guard command (or dropping the key — which then needs
     `metadata/schemas/bootstrap-v1.schema.json:81`'s `required` list and
     both bootstrap scripts edited in the same commit) turns onboarding
     itself into a hard failure for every new contributor, not a Phase-0
     nicety. `python scripts/check_bootstrap_manifest.py` passing is this
     phase's exit criterion for that piece, not merely "west_commands
     deleted cleanly."
   - **`pyproject.toml`** — `[tool.setuptools.packages.find]` (:57–59) has
     `include = ["alp_cli*", "alp_mcp*"]`, and `[project.scripts]` (:51–52)
     has `alp-mcp = "alp_mcp.server:main"`. Deleting `alp_cli/**` (this
     phase) leaves the `include` list, and the still-open `alp_mcp`
     question, pointed at a missing package; `metadata/bootstrap.json:48`'s
     `"editableInstall": "${SDK_ROOT}"` is what pip-installs this
     distribution during bootstrap, so a stale `include`/`[project.scripts]`
     is a bootstrap-time failure too, not just an unused metadata field. The
     comment at :47–50 ("The `alp_cli` package stays as tan's Python
     backend...") retires in the same commit — **but only after Phase 0a. An
     earlier draft of this bullet called that comment stale on the strength of
     `tan-cli/python/tan/commands/sdk_cmd.py:38` ("Nothing here runs `python -m
     alp_cli`"); that was a scope misread and the comment is currently
     ACCURATE.** Line 38 sits inside `sdk_cmd.py`'s own docstring and scopes to
     the `sdk` verb; `crates/tan-cli/src/commands/sdk_cli.rs` is a different
     file that spawns `python -m alp_cli` for four verbs, and
     `crates/tan-cli/src/commands/bootstrap/steps.rs:582-602` pip-installs this
     distribution editable as "tan's Python backend (alp_cli)". Dropping
     `alp_cli*` from :59's `include` before Phase 0a therefore makes `tan
     bootstrap` install a distribution missing the backend the same binary then
     spawns. See Hard Fact 5.
   - **`.github/workflows/pr-alp-build.yml:191,238`** — both spawn
     `PYTHONPATH="$PWD/scripts" python -m alp_orchestrate --input ...
     --emit system-manifest`, masked with `|| true` but caught by the
     following step's `test -s "${manifest}" || exit 1` (:200–204). This is
     a live workflow spawn of `alp_orchestrate` by path, not an `import` —
     §2's "exactly 7" import-coupled count above does not include it because
     it isn't a `check_*.py`, but Phase 3 deletes the thing it spawns, so
     this workflow must be repointed at a `tan` equivalent no later than
     Phase 3, and doing it here (alongside the other front-door work) keeps
     the repoint in the same commit as the rest of Phase 2's CI rewiring
     rather than a separate pass.

   This is safe to do in parallel with Phase 1 (disjoint file sets) but must
   complete before Phase 3, because these front doors are exactly what still
   imports the 28 planner/emit modules for a *live* purpose beyond gating.

4. **Phase 3 — delete the 28 planner/emit modules** (`scripts/alp_orchestrate/**`,
   `scripts/alp_project_emit/**`, `scripts/alp_project_loader.py`,
   `scripts/alp_registries.py`) plus `scripts/alp_template.py`,
   `scripts/gen_zephyr_board.py`, `scripts/build_receipt.py`, and their ~44+
   tests — now that Phase 1 rewired their last real callers inside `scripts/`
   and Phase 2 removed the front doors. `alp_project.py` is explicitly
   **excluded** from this phase (it is the 29th file — see the "28 +
   BLOCKED" naming above).

   Two more prerequisites, both cross-repo, both missed by an earlier draft
   of this document:
   - **tan-cli's `looks_like_sdk_checkout`** (`<tan-cli>/tests/parity/_sdk_checkout.py:32`,
     `return (path / "scripts" / "alp_orchestrate").is_dir()`) gates four
     tan-side byte-parity scripts (`toolchain_lock_parity.py`,
     `bootstrap_manifest_parity.py`, `kconfig_fixture_parity.py`,
     `scaffold_byte_parity.py`; see Hard Fact 3, item 4, and Hard Fact 2 for
     why this makes `scripts/alp_orchestrate/` a second cross-repo marker
     contract). tan-cli's own CI (`.github/workflows/parity.yml:191,201,211,220`,
     each passing `--sdk alp-sdk`) already hard-FAILs once the directory is
     gone — good, that is caught — but this only holds because tan-cli#172
     turned an explicit `--sdk` into a demand rather than a silent
     fall-through; the local no-`--sdk` dev loop still SKIPs/exit-0. Before
     `scripts/alp_orchestrate/**` is deleted, `looks_like_sdk_checkout` must
     be repointed at a marker that survives the deletion (or the local SKIP
     path made a hard failure too) — otherwise this phase breaks four
     tan-side CI jobs with only the `--sdk`-flagged CI runs there to notice.
   - **`scripts/alp_template.py`'s scaffold survival** (Phase 1's
     precondition above) must already be in place: this phase deletes
     `alp_template.py` itself, so if Phase 1 retired `check_emit_snapshots.py`
     wholesale instead of scoping the retirement to tan-covered modes, this
     phase would remove the last SDK/Rust/Python parity chain keeping tan's
     25 hardware-fact-bearing vendored scaffold files (e.g.
     `python/tan/templates/vendored/diagnostics/E1M-V2N101/board.yaml`,
     carrying `sku: E1M-V2N101`, `e1m: E1M_X_I2C0`, part numbers `ICM-42670,
     BMI323, BMP581, TCAL9538, INA236`) honest against drift.

5. **Phase 4 — the part that never fully closes.** `scripts/alp_project.py`
   and `scripts/kconfig/alp_kconfig_dump.py` cannot be deleted at the end of
   Phase 3 the way everything else can (Hard Facts 2 and 4). The most this
   plan can do to either is shrink them: `alp_project.py` down to the CLI
   dispatch + whatever the `TAN_GENERATE_EXECUTOR=subprocess` fallback needs
   (which, once `alp_project_loader.py`/`alp_project_emit/` are deleted per
   Phase 3, means `alp_project.py` needs its own inlined copy of that logic —
   Phase 3 and Phase 4 are in tension on this one file and must be sequenced
   as "inline first, delete the package second," not the reverse);
   `alp_kconfig_dump.py` down to an inlined `_project_symbols` that does not
   import the (by-then-deleted) `alp_orchestrate.kconfig_symbols`. Both stay
   in the tree indefinitely, or until Alp Lab is willing to break every `tan`
   binary that predates whatever replaces the marker-file/subprocess-hook
   mechanisms — which is a product decision, not an engineering one.

   The "inline first" requirement above is not just a design preference —
   `scripts/alp_project.py --emit <mode>` is spawned directly, by path, from
   three live CI workflows, which is exactly what continues to exercise the
   inlined logic once Phase 3 removes `alp_project_loader.py`/
   `alp_project_emit/`: `cross-platform-zephyr.yml:197,200,203,206,209,212`
   (`zephyr-conf`, `cmake-args`, `hw-info-h`, `dts-overlay`,
   `west-libraries`, `yocto-conf`), `pr-metadata-validate.yml:135,144,153,166,197,215`,
   and `pr-tier-a-libraries.yml:184`. None of these need editing for Phase 4
   to land — they call `alp_project.py` by path today and keep doing so
   afterward — but they are the reason an inlining mistake surfaces as a
   live CI break on the next PR, not a silent gap.

## 3. The six hard facts

### Hard Fact 1 — every example build takes the Python fallback today

`cmake/alp.cmake:85` probes `find_program(ALP_SDK_TAN_PROGRAM NAMES tan)`;
`cmake/alp.cmake:113-138` uses it only if found *and* its `generate --help`
output matches `--output` (line 94). Failing either, `_alp_sdk_emit()` falls
back to `${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py`
(line 131), logged via `message(STATUS "alp-sdk: --emit ${mode} via
${_alp_sdk_via}")` (line 141).

Across all 25 `.github/workflows/*.yml`, exactly **one** installs `tan`:
`tan-docs-drift.yml:59-63`, via
`curl -fsSL -o /tmp/tan-install.sh https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh`.
That job is explicitly advisory-only (its own header comment, lines 19–22)
and runs `scripts/check_tan_docs_surface.py` — a docs-prose check, not a
CMake configure. None of the workflows that actually build examples through
`cmake/alp.cmake` — `pr-twister.yml`, `cross-platform-zephyr.yml`,
`pr-getting-started-aen801.yml`, `pr-generated-files.yml`,
`pr-metadata-validate.yml`, `pr-tier-a-libraries.yml` — mention `tan`
anywhere (`grep -nE '\btan\b'` on each returns nothing; the naive `grep -n
tan` is not safe to use here — it false-positives twice inside
`pr-metadata-validate.yml:118,120`, both matches landing inside the word
"standard"). So on every one of them,
`find_program(tan)` fails, `ALP_SDK_TAN_EMITTER` stays empty, and every
`alp_sdk_zephyr_conf()` call goes through the Python fallback. `cmake/alp.cmake:8`'s
header comment cites "96 example `CMakeLists.txt` files"; the checkout today
carries 99 `board.yaml` files (`git ls-files 'examples/**/board.yaml'`) — the
comment predates growth, immaterial to the finding: **100% of them, right
now, take the Python branch.**

What would unblock removing the fallback: (1) a CI step, on every
example-building workflow, that installs a pinned `tan` before configure —
the exact one-liner `tan-docs-drift.yml` already proves works; and (2)
resolving whether that install path is acceptable as alp-sdk's *only* route
to a working build (today it is a raw, unauthenticated GitHub-hosted shell
script with no supply-chain pinning beyond `main`, which is a materially
weaker guarantee than the vendored Python it would replace).

One nuance worth flagging rather than asserting past: the task's framing
states "tan-cli is private." `tan-docs-drift.yml:61`'s
`curl -fsSL https://raw.githubusercontent.com/alplabai/tan-cli/main/install.sh`
is an **unauthenticated** fetch, and `raw.githubusercontent.com` returns 404
for a private repo's raw content without a token in the request (none is
supplied here). Combined with that job's own comment that it "runs green
against `dev`" today, this suggests `tan-cli` (or at minimum its
`install.sh`) is publicly fetchable right now — which would partially
contradict a strict "private repo, no public install path" framing. This
worktree has no way to independently query `alplabai/tan-cli`'s GitHub
visibility setting, so this is flagged as worth confirming, not asserted
either way; it does not change Hard Fact 1's finding (no CI job installs
`tan` for the *build* path), only the "why not" for a fix.

### Hard Fact 2 — `alp_project.py` is a compiled-in root marker; no checkout-side fix reaches v0.4.0

`scripts/alp_project.py` is not just a loader — it is a **discovery
anchor**. `tan-cli/crates/tan-cli/tests/sdk_ancestor_discovery.rs:35-36`
states the contract directly: *"`scripts/alp_project.py` is the only marker
discovery probes, so an empty file is a complete stand-in for a checkout as
far as this test is concerned"* — and the test's own `make_sdk_root()`
function (lines 37-40) creates nothing but an empty `scripts/alp_project.py`
to pass SDK-root discovery. `tan-cli/crates/tan-core/src/loader.rs:255` joins
`sdk_root/scripts/alp_project.py` to build the fallback command line, and
`tan-cli/CHANGELOG.md:30` ("`scripts/alp_project.py` remains tan's canonical
SDK-root marker") confirms this is a stated, permanent contract, not an
accident of the current branch.

This appears at roughly **15 sites across the tree and its siblings** —
`cmake/alp.cmake`, `docs/board-config*.md`, `docs/superpowers/specs/*`,
`scripts/check_zephyr_conf_parity.py`, `scripts/alp_template.py`,
`tests/fixtures/emit-snapshots/*.snap` (5+ fixture files), plus the tan-side
`loader.rs`, `sdk_ancestor_discovery.rs`, `build_readiness.rs` (7 distinct
comment/code sites), `CHANGELOG.md` (11+ historical references), and
`llms.txt` — spanning alp-sdk and tan-cli, i.e. genuinely "three codebases"
once alp-sdk-vscode's own SDK-discovery logic (out of scope for this
worktree to inspect) is counted as the third.

What this forces: **a `tan` binary already built and shipped against this
marker contract — `tan-cli`'s own changelog dates it to v0.4.0's discovery
logic — will fail SDK-root discovery the moment `scripts/alp_project.py` is
deleted, no matter what alp-sdk does on its own side.** There is no
checkout-side compatibility shim possible for a binary already in a
customer's hands; the only ways out are (a) never delete the file (Phase 4's
conclusion above), (b) ship a new discovery marker in a future `tan` release
and treat every pre-that-release `tan` as end-of-life, or (c) both — add a
second marker now, drop the old one only after a deprecation window long
enough that no supported `tan` still probes for it alone.

`scripts/alp_project.py` is not the only such marker. `scripts/alp_orchestrate/`
is a second, weaker one: `<tan-cli>/tests/parity/_sdk_checkout.py:32`
(`looks_like_sdk_checkout`) probes for `scripts/alp_orchestrate/` as its
sole test of "is this an alp-sdk checkout", gating the four tan-side
byte-parity scripts named in Hard Fact 3, item 4 below. It differs from the
`alp_project.py` marker in one important way: it is a source-level Python
check inside tan-cli's own test suite, not something compiled into a shipped
`tan` binary — so it can be repointed by editing tan-cli, with no
customer-binary compatibility problem. It still has to be repointed (or its
SKIP-on-no-`--sdk` path closed) before this document's Phase 3 deletes the
directory it probes for, which is why it belongs in the same category as
this Hard Fact even though its fix is unilateral rather than a deprecation
window.

### Hard Fact 3 — five gates that would go vacuously green (or vacuously dead), not red

1. **`scripts/check_doc_drift.py:239`**: `harvest_tree(root / "scripts", "*.py")`
   (the executing call; the docstring describing it is at line 205:
   `* generators / tooling     scripts/**/*.py  (board names, ...)`). Once
   every file under `scripts/` is deleted, `base.rglob("*.py")` at line 221
   matches **zero files**. `harvest_tree` has no "found nothing" branch — it
   silently contributes an empty set to `collect_known_symbols()`'s "generators
   / tooling" source layer. The gate does not fail; it just stops being able
   to recognise any identifier that used to be defined only in `scripts/`
   (board names, `ALP_HW_BUILD_*`/`ALP_SOC_*` macros the emitters mint), which
   makes docs describing those identifiers **look like drift when they are
   not**, or — worse — lets real drift in a surface this layer used to catch
   pass silently once the false positives train someone to `--allow` past
   this check's output.

2. **`scripts/check_tan_docs_surface.py:231`**:
   `_OTHER_FRONT_DOOR_RE = re.compile(r"python -m alp_cli|west alp-[a-z][a-z-]*|alp_orchestrate")`.
   This regex exists to recognise, inside `docs/cli.md` **prose**, when a
   table row is documenting some *other* front door's flags rather than the
   section's own `tan <verb>`. It matches the literal text `alp_orchestrate`
   wherever it appears in the Markdown source — it never imports, calls, or
   otherwise verifies that `alp_orchestrate` exists as working code. Deleting
   `scripts/alp_orchestrate/` from the tree does not change one byte of what
   this regex matches; the check keeps "passing" (in the sense of correctly
   classifying doc rows) while documenting a module that no longer exists
   anywhere to run.

3. **`tests/scripts/test_dispatch_paths_match_seam1.py:53`**:
   `assert sorted(sender) == sorted(seam1), (...)`, comparing
   `.github/workflows/dispatch-tan-parity.yml`'s and
   `.github/workflows/parity-seam1.yml`'s `on.push.paths` lists. Both files
   list `"scripts/alp_orchestrate/**"` as their first watched path
   (`dispatch-tan-parity.yml:47`, `parity-seam1.yml:17` and `:24`). If
   `scripts/alp_orchestrate/` is deleted, that glob starts matching nothing in
   **both** files simultaneously — the assertion at line 53 still holds
   (`sorted(sender) == sorted(seam1)` is unaffected by a shared dead entry),
   so the test stays green while the trigger it exists to keep in lockstep
   has gone quiet on both sides at once. The `len(sender) >= 4` floor
   (module docstring, further down the file) does not catch this either,
   since deleting one path still leaves 3 live ones (`metadata/**`,
   `examples/**/board.yaml`, `tests/parity/**`).

4. **`<tan-cli>/tests/parity/_sdk_checkout.py:32`** (tan-cli side, not
   alp-sdk's own gates): `def looks_like_sdk_checkout(path): return (path /
   "scripts" / "alp_orchestrate").is_dir()` — the shared root-resolution
   helper for four tan-side byte-parity gates its own module docstring names
   (lines 3-5): `toolchain_lock_parity.py`, `bootstrap_manifest_parity.py`,
   `kconfig_fixture_parity.py`, `scaffold_byte_parity.py`. Two behaviors,
   split by whether `--sdk` was passed. `.github/workflows/parity.yml`
   (tan-cli) passes `--sdk alp-sdk` to all four (`:191,201,211,220`), and
   after `scripts/alp_orchestrate/**` is deleted that path correctly
   hard-FAILs (`_sdk_checkout.py:75-81`, message *"FAIL: --sdk {resolved}
   does not look like an alp-sdk checkout (no scripts/alp_orchestrate/ dir)
   ..."*) — CI itself is not fooled, thanks to the explicit-`--sdk`-is-a-demand
   fix from tan-cli#172. But the no-`--sdk` local dev-loop path
   (`resolve_sdk_root` returns `None`, `_sdk_checkout.py:84-86`) prints SKIP
   and returns `0`: run any of these four gates locally without `--sdk` and
   they pass having checked nothing — the same shape as items 1-3 above.
   This makes `scripts/alp_orchestrate/` a SECOND cross-repo marker contract
   alongside Hard Fact 2's `scripts/alp_project.py` (recorded there too);
   see Phase 3's prerequisites in §2 for what must happen before this
   directory can be deleted.

5. **`.github/workflows/pr-generated-files.yml:54`** and
   **`.github/workflows/pr-metadata-validate.yml:28`** both carry a
   `'scripts/alp_orchestrate/**'` `on.push.paths` filter, and
   `pr-metadata-validate.yml:49` and `:82` both carry a
   `'scripts/alp_cli/__init__.py'` filter — the same dead-trigger shape as
   item 3 above (a path glob that matches nothing once Phase 2/Phase 3
   delete the directories), but unguarded: `test_dispatch_paths_match_seam1.py`
   only compares `dispatch-tan-parity.yml`'s and `parity-seam1.yml`'s path
   lists against *each other*, never against these two workflows. Once both
   directories are gone, `pr-generated-files.yml` and `pr-metadata-validate.yml`
   each keep a permanently-dead trigger entry with nothing that fails to
   flag it.

No further gates were found matching this shape beyond the five now listed;
the closest additional candidate, `scripts/check_quality_registry.py`, was
checked and does not glob `scripts/` or regex-match a module name — it
cross-references `metadata/quality-tasks-v1.json` task IDs against
`check_*.py` **filenames**, which is a real presence check (a missing file
fails it), not a vacuous one.

### Hard Fact 4 — the two "safe survivors" both import doomed modules today

`scripts/kconfig/alp_kconfig_dump.py:48`:
```python
from alp_orchestrate.kconfig_symbols import _project_symbols  # noqa: E402
```
`scripts/alp_project.py:80` and `:96`:
```python
from alp_project_loader import (  # noqa: F401  (compat re-export)
    METADATA_ROOT, _compose_route, _load_yaml, _resolve_board,
    _resolve_inline_or_preset_board, _resolve_pad_routes,
    _resolve_silicon_variant, _resolve_sku, _sku_family,
    _validate_and_load, resolve_capabilities, resolve_memory_map,
    silicon_to_kconfig, som_unpopulated_capabilities,
)
from alp_project_emit import (  # noqa: F401  (compat re-export)
    _CHIP_SUBSYSTEMS, _PERIPHERAL_KCONFIG, _SOC_FAMILY_TOKEN,
    _emit_carrier_netlist, _emit_composed_route_table, _emit_dts_overlay,
    _emit_hw_info_h, _emit_library_hw_backends, _emit_native_sim_overlay,
    _emit_west_libraries,
)
```

Confirmed exactly as the task states. Both files must remain runnable as
standalone scripts (§1.1's BLOCKED rows explain why: one is spawned by
Zephyr's `EXTRA_KCONFIG_TARGET` CMake hook with a literal, non-redirectable
command line; the other is `tan`'s own root marker and subprocess fallback).
Neither can be left untouched once `alp_orchestrate`/`alp_project_loader`/
`alp_project_emit` are deleted — they will `ImportError` at the exact moment
Zephyr or `tan` spawns them, which is a hard runtime failure, not a lint
warning. This directly contradicts treating either as a "safe survivor" that
needs no further work: **Phase 4 (§2) must inline each file's actual runtime
dependency before Phase 3 deletes the package it currently imports from —
the two phases are coupled on these two files specifically, in the opposite
order from every other row in this document.**

### Hard Fact 5 — the shipped `tan` is a front end TO `alp_cli`, not a replacement for it

This is the fact that turns §1.2's `alp_cli` row from DELETE into BLOCKED, and
it is stated by the Rust source itself. `tan-cli crates/tan-cli/src/commands/sdk_cli.rs:2-6`
(worktree `python-executor`):

> Thin forwarders to the SDK's `alp` click CLI (`python -m alp_cli <sub>`):
> `tan model` / `tan monitor` / `tan new-som` / `tan faultdecode`.
>
> Under ADR-0020 the model packaging, serial console, SoM scaffolding and
> fault-decode arithmetic all live in the SDK (`scripts/alp_cli/`); `tan` is
> only the command surface.

Those forwards are live, not dead code: registered at
`crates/tan-cli/src/cli.rs:258,260,262,264` and dispatched at
`crates/tan-cli/src/main.rs:69-72` as
`commands::sdk_cli::run(&global, "model"|"monitor"|"new-som"|"faultdecode", …)`,
with the argv assembled at `sdk_cli.rs:43-52` as `python -m alp_cli <sub>`.
The behaviour behind them is **1367 lines** with no counterpart in either
repo — `new_som.py` 645 + `faultdecode.py` 598 + `monitor.py` 73 +
`model.py` 51, per `wc -l scripts/alp_cli/*.py` (`3998 total` for all 19).
Corroborating absences on the tan side, each a command whose output is the
whole evidence:

```
$ grep -rn "CFSR\|HardFault\|faultdecode\|HFSR" --include=*.py .   # in tan-cli/python/tan/
./planner/zephyr_board.py:487:        "# a garbage SP/PC from the 0x600 padding gap -> instant HardFault -> reset/reboot\n"
$ grep -rln "alpmodel\|alp_model" --include=*.py .                 # in tan-cli/python/tan/
(no output)
```

A sixth surface is coupled the same way one level down: `tan validate` is
implemented by an alp-sdk script (`crates/tan-cli/src/commands/validate.rs:5,134-145`
spawns `<sdk>/scripts/validate_board_yaml.py --input <board>`), which is
itself inside Hard Fact 6's break set.

**The packaging is coupled too, and `pyproject.toml`'s comment is accurate
rather than stale.** `crates/tan-cli/src/commands/bootstrap/steps.rs:582-602`
pip-installs this distribution editable, logging "tan's Python backend
(alp_cli) -- editable" and "Installing the tan CLI's Python backend into the
venv (pip install -e …)", and warning `"alp_cli editable install reported a
problem -- check manually"` on failure. So `pyproject.toml:47-50` —

> No `alp` console-script: `tan` (the standalone Rust CLI) is the sole
> user-facing command surface (ADR-0020 end-state B). The `alp_cli` package
> stays as tan's Python backend, invoked as `python -m alp_cli <sub>` — never
> as a user-installed `alp` binary.

— is a **current, correct** description of the architecture, and dropping
`alp_cli*` from `pyproject.toml:59`'s
`include = ["alp_cli*", "alp_mcp*"]` would make `tan bootstrap` install a
distribution missing the very backend the same binary then spawns. An earlier
draft of this document called that comment stale on the strength of
`tan/commands/sdk_cmd.py:38` ("Nothing here runs `python -m alp_cli`"); that
was a scope misread — line 38 sits inside `sdk_cmd.py`'s own docstring and
scopes to the `sdk` verb alone. `sdk_cli.rs` is a different file and does
exactly what `sdk_cmd.py` disclaims.

### Hard Fact 6 — `alp_cli.validator` is the planner's own schema implementation, imported at module scope

`scripts/alp_cli/` is not only a front door. `scripts/alp_cli/validator.py:10-15`:

> This module is also the ONE shared board.schema.json implementation:
> `load_board_schema()` / `iter_schema_errors()` are consumed by
> `scripts/validate_board_yaml.py` (the customer-side pre-flight validation
> CLI) and `scripts/alp_orchestrate/` (the plan/emit loader), so the schema
> file, draft dialect, and error ordering are decided in exactly one place.

`scripts/alp_orchestrate/loader.py:29` acts on that at **module scope** —
`from alp_cli.validator import iter_schema_errors` — so the break is at import
time, not at call time, and it propagates to everything that imports the
planner: `git grep -lE '^\s*(from|import)\s+alp_orchestrate' -- '*.py' | wc -l`
→ `34`, of which three are STAYS-IN-ALP-SDK gates
(`git grep -lE '^\s*(from|import)\s+alp_orchestrate' -- 'scripts/check_*.py'`
→ `check_build_plan.py`, `check_system_manifest.py`,
`check_zephyr_conf_parity.py`) and 22 are tests
(`… -- 'tests/**/*.py' | wc -l` → `22`).

Verified empirically rather than by reading imports, with a `sys.meta_path`
finder that makes any `alp_cli` import raise `ModuleNotFoundError` — a
simulated deletion, no file touched:

```
BREAK alp_orchestrate: ModuleNotFoundError: simulated deletion: no module named 'alp_cli'
BREAK alp_orchestrate.loader: ModuleNotFoundError: simulated deletion: no module named 'alp_cli'
BREAK validate_board_yaml: ModuleNotFoundError: simulated deletion: no module named 'alp_cli'
OK    alp_project_loader
OK    alp_project
```

`alp_project_loader` and `alp_project` survive because
`scripts/alp_project_loader.py:140-141` does its `from alp_cli.validator
import validate_board_yaml` / `from alp_cli.diagnostic import render` **inside
a `try:`/`except ImportError`** with a documented fallback to "the legacy bare
loader so existing workflows that haven't installed the dev extras keep
working; they just won't get rich diagnostics". That is a graceful
degradation, not immunity: deleting `alp_cli` silently downgrades every
board.yaml diagnostic that path produces. `alp_orchestrate/loader.py:29` has
no such guard.

The consequence for the phase order: `alp_cli/{validator,diagnostic,yaml_pos}.py`
cannot be deleted in the same phase as the front-door verb modules. They are a
library that the MOVES-TO-TAN planner still consumes, and tan has already
relocated their content — `python/tan/planner/loader.py:11-12` says
`load_board_schema`/`iter_schema_errors` are "RELOCATED from alp-sdk's
`scripts/alp_cli/validator.py`, the last module-scope import this file made" —
so the fix is to finish that relocation on the alp-sdk side, not to delete
ahead of it.

## 4. Byte-parity oracle per MOVES-TO-TAN row

alp-sdk's own committed generated tree is the only artefact that can prove a
relocated generator did not drift, because it is the one thing both the old
(subprocess `alp_project.py`/`alp_orchestrate`) and new (in-process
`tan.planner`) code paths must reproduce byte-for-byte. Concretely, per
group:

- **`alp_orchestrate/*` (buildplan, manifest, headers, kconfig, secure,
  carveout, partition, orchestrator, topology, models, libraries, slugs,
  validate, memregion, paths, cli):** `tan-cli/python/tests/parity/
  test_planner_emit_parity.py` (in-process `tan.planner` vs. subprocess
  `python -m alp_orchestrate`, diffed over every `board.yaml` under
  `examples/**` × every `--emit` mode) is the oracle already built for this;
  alp-sdk's own `scripts/check_emit_snapshots.py` against
  `tests/fixtures/emit-snapshots/*.snap` is the second, independent oracle
  (compares live emit to a committed golden snapshot rather than to a sibling
  process) and should keep running until the tan-side oracle has covered
  every mode `check_emit_snapshots.py` covers, per its own `ORCH`/`PROJ`
  dual-invocation design (lines 41–42).
- **`alp_project_loader.py`, `alp_project_emit/*`, `gen_zephyr_board.py`,
  `alp_template.py`, `alp_registries.py`:** same two oracles — they are
  exactly the modules `check_emit_snapshots.py` and
  `test_planner_emit_parity.py` already exercise (the docstring at
  `check_emit_snapshots.py:52-56`, quoted verbatim: *"PROJ (`alp_project.py`)
  -- the single-core `--emit` surfaces rendered by `alp_project_emit.py`
  (dts overlay, native-sim overlay, hw-info header, west libraries, carrier
  route/netlist) plus `zephyr-conf` (alp.conf for a single core) and
  `os-topology` (the `loader.py::load_board_yaml` resolution)"* — note this
  list does not include `kconfig` or `scaffold`; `scaffold` in particular has
  no tan-side oracle at all yet, per Phase 1's precondition in §2).
- **`build_receipt.py`:** `scripts/check_build_receipt.py` (schema
  well-formedness) plus a new byte-diff of the receipt's `alp.lock`-derived
  digest across old/new code paths — no existing test does the cross-path
  diff for this one specifically; it would need a `test_planner_emit_parity.py`-style
  addition on the tan side.
- **`alp_model/*` (build, manifest, package, targets, tensorio, adapters/*):**
  **no existing byte-parity oracle** — `tests/fixtures/models/gen_tiny_model.py`
  and `gen_tiny_onnx.py` are today's only fixtures, and today's tests
  (`test_alp_model_*.py`) exercise only the alp-sdk-side implementation.
  Building the oracle means: capture a golden `.alpmodel` package (or at
  minimum its manifest + per-blob digests, since some blobs are
  toolchain-nondeterministic across host installs) from today's
  `alp_model.build` for each fixture × each SoM target, then diff a future
  tan-side implementation's output against that golden set the same way
  `test_planner_emit_parity.py` does for the emit modes. This is unstarted
  work (§2, Phase 0b) — the byte-parity harness cannot be written until
  there is a second implementation to diff against.
