# What the tan Python Port Must Preserve

**Baseline:** `alp-sdk` worktree `E:\GitHub\alp-sdk\.claude\worktrees\tan-python-port` @ `3050a142` (branch `design/tan-python-port`); `tan-cli` `E:\GitHub\tan-cli` @ `94a6ffd` (branch `dev`); `alp-sdk-vscode` `E:\GitHub\alp-sdk-vscode`. Every claim below was re-verified against these trees. Line numbers are from this baseline and drift; the quoted text is the durable anchor.

**Enforcement vocabulary used throughout:**
- **BLOCKING** — inside a required status check. Only two contexts on `main` (`twister · native_sim/native/64`, `clang-format · diff-only`, `strict:true`) and six on `dev` (`clang-format · diff-only`, `twister-shard 1/4`..`4/4`, `renode · V2N101 --sim-mode socket contract`, `strict:false`) — read live via `gh api repos/alplabai/alp-sdk/branches/{main,dev}/protection`.
- **CI (non-blocking)** — runs in a workflow, reddens the UI, does not stop a merge.
- **LOCAL-ONLY** — a `gate: true` registry entry with no workflow and no pytest coverage.
- **DOCUMENTED-ONLY** — prose or a comment. **These are the ones that rot.**

**Correction that governs the whole document:** `gate: true` in `metadata/quality-tasks-v1.json` does *not* mean "runs in CI", and `ci: null` does *not* mean "runs nowhere". Eleven of 42 tasks are `gate:true, ci:null` (`board-schema-version, bootstrap-manifest, build-plan, cmake-chip-list-parity, diagnostic-schema, emit-registry, example-storage-claims, system-manifest, template-catalog, toolchain-lock, zephyr-conf-parity`) — but `.github/workflows/pr-metadata-validate.yml:378` runs `python3 -m pytest tests/scripts/ -v --tb=short`, and I verified live-corpus assertions there for **nine of the eleven**: `tests/scripts/test_check_build_plan.py:29 test_default_corpus_conforms`, `test_check_system_manifest.py:29 test_default_corpus_conforms`, `test_check_emit_registry.py:36/46 test_committed_registry_conforms / test_real_emit_modes_matches_registry_exactly`, `test_check_bootstrap_manifest.py:99 test_default_corpus_passes`, `test_check_example_storage_claims.py:92 test_live_repo_is_clean`, `test_check_zephyr_conf_parity.py:38 test_default_corpus_byte_identical`, `test_check_template_catalog.py:44 test_committed_catalog_conforms`, `test_check_toolchain_lock.py:125 test_default_corpus_passes`. `board-schema-version` and `cmake-chip-list-parity` have test files whose live-corpus coverage I did not confirm. `diagnostic-schema` has no `test_check_diagnostic_schema.py` — **cannot determine** whether it runs anywhere; treat as LOCAL-ONLY.

**Derive enforcement from workflow `run:` steps plus `tests/scripts/`, never from the registry's flags.** Neither `scripts/check_quality_registry.py` (which only asserts on-disk↔registry name parity, `check_quality_registry.py:33-34`) nor anything else validates the `ci` field, so it is stale in both directions.

---

## 1. THE INVARIANTS

### 1.1 CRITICAL — breaking these ships wrong firmware or strands a consumer

---

**I-01 — The OS is derived from the core's Cortex class and is never selectable.**

`cortex-a*` → `yocto`, `cortex-m*` → `zephyr`, anything else → `off`. A `board.yaml` may set only `off` (skip the slice) or `baremetal` (no-OS firmware); naming the *other* class's OS is a hard load error.

- `scripts/alp_orchestrate/topology.py:38-42` — `t = (core_type or "").lower()` / `if t.startswith("cortex-a"): return "yocto"` / `if t.startswith("cortex-m"): return "zephyr"` / `return "off"`
- `scripts/alp_orchestrate/validate.py:249-255` — raises `OrchestratorError(f"core '{slice_.core_id}' ({core_type or 'unclassified'}): its runtime is determined by the core class (Cortex-A -> Yocto/Linux, Cortex-M -> Zephyr/RTOS) and is not selectable. Set os: 'off' to disable it or 'baremetal' for no-OS firmware -- got os: '{slice_.os}'.")`
- Called per core at `loader.py:369-370`.

**ENFORCED (code) + BYTE-PINNED** (`proj-*.os-topology.snap`).
**Port rule:** tan receives an already-resolved `slices[].backend ∈ {zephyr, yocto, baremetal}`. Never expose `--os` or `--backend`. Keep the cross-class rejection *and* the exact message. Note the nuance every ADR blurs: the enum has four values and a customer *can* legally type two of them.

---

**I-02 — A `board.yaml` may never carry a top-level `os:` key. The schema rejects the document outright.**

`metadata/schemas/board.schema.json` top level: `"not": {"required": ["os"]}` (verified via schema load).

**ENFORCED (schema)**, applied by `scripts/validate_board_yaml.py`, run at `pr-metadata-validate.yml:233-257` over the canonical template, every `examples/*/board.yaml`, and a sweep across `examples/` + `tests/`.
**Port rule:** reject before planning. Do not "helpfully" accept and forward.

---

**I-03 — `board`, `machine`, `toolchain`, `hw_console` are SoM-preset facts. `cores.<id>` cannot carry them.**

`board.schema.json` `$defs/core_entry` is `additionalProperties: false` with exactly `['app','extra_libraries','image','inference','iot','memory','os','peripherals','power','recipe']` (verified). The four missing keys live in `som-preset-v1.schema.json` `$defs/topology_entry` and are read at `loader.py:196-209`.

**ENFORCED (schema).**
**Port rule:** resolve those four exclusively from the SoM preset. Loosening `core_entry` silently lets a customer pick a Zephyr board target the SoM was never brought up on.

---

**I-04 — Heterogeneous-by-default: the planner fans out over the *SoC's* `cores[]`, not over what the customer typed.**

`loader.py:349` — `for core_id in soc_core_ids:` where `soc_core_ids = [c["id"] for c in (soc_spec.get("cores") or []) if "id" in c]` (`loader.py:300`). A core never mentioned in `board.yaml` still becomes a slice from the SoM preset's `topology.<id>` defaults — a Yocto `bitbake alp-image-edge` slice on the A-cluster (`orchestrator.py:84 STOCK_IMAGE_APP`), an `alp-stock-shim` Zephyr slice on an idle M peer (`orchestrator.py:74`).

Verified live: `examples/peripheral-io/gpio-button-led/board.yaml` names only `m55_he` and plans **three** slices — `a32_cluster/yocto`, `m55_he/zephyr`, `m55_hp/zephyr`. Of 99 example `board.yaml` files, 97 sit on a SoC with an A-core and **40** never name it.

**ENFORCED (code)** + byte-pinned for the four `rpmsg-*`/`heterogeneous-offload` cases.
**Port rule:** fan out over SoC cores and materialise `${SDK_ROOT}/firmware/alp-stock-shim`. This interacts with `validate.py:262-266` (`os: zephyr` requires `app:`) — the preset's stock-shim default is *load-bearing*: a new SoM preset that omits `app:` on a peer core breaks every customer `board.yaml` for that SKU at load time.

---

**I-05 — Every key under `cores:` must exist in the SKU's topology. One typo is a hard error, never warn-and-drop.**

`loader.py:336-339` — `raise OrchestratorError(f"board.yaml \`cores:\` declares unknown core id(s) {unmatched} that {sku}'s \`topology:\` does not expose. Did you mean one of: {sku_topology}?")`. Rationale in-file (#603): pre-fix, a misspelled core silently vanished from the build while the file still validated clean.

**ENFORCED (code)**, covered by `tests/scripts/test_orchestrate_loader.py`.

---

**I-06 — TWO DIFFERENT SLICE ORDERINGS. Do not unify them.**

- **build-plan**: `sorted(coreId)`, `off` cores excluded — `orchestrator.py:34-38`.
- **system-manifest**: SoC `cores[]` **array order**, `off` cores **included** — `manifest.py:37` `effective_slices = list(slices) if slices is not None else list(project.cores.values())`, over a dict built by `loader.py:349`.

Proof in the frozen goldens: `tests/fixtures/emit-snapshots/rpmsg-aen.system-manifest.snap` lines 10/20/28 = `a32_cluster` / `m55_hp` / `m55_he`; `rpmsg-aen.build-plan.snap` lines 17/65/119 = `a32_cluster` / `m55_he` / `m55_hp`.

**BYTE-PINNED ONLY.** `check_som_topology_parity.py` compares *sets*, so reordering `cores[]` inside a `metadata/socs/**/*.json` passes every declared gate and silently rewrites every manifest's bytes.
**Port rule:** two enumeration rules, one per emitter. A single shared `slices()` helper gets one of them wrong and only the goldens notice.

---

**I-07 — `schemaVersion` is `const: 1` and is a hard version handshake, not a hint.**

`metadata/schemas/build-plan-v1.schema.json:10-13`; consumer side `tan-cli/crates/tan-core/src/build_plan.rs:30 BUILD_PLAN_SCHEMA_VERSION = 1`, probed **first on a minimal struct** (`build_plan.rs:327`) and rejected with `unsupported build-plan schemaVersion {found} (this CLI consumes v{supported})` (`:311`), round-trip tested at `:622`.

Schema description: *"schemaVersion 1 is locked with the tan-cli consumer -- bump schemaVersion and record the change in the CHANGELOG before altering this shape; do not add breaking changes silently."*

**ENFORCED both sides** (schema const + live reject path + cargo test).
**Port rule:** do **not** bump during the port. Keep the skew guard even when planner and executor ship together — that redundancy is exactly what RFC #843 fixed, and single-repo is precisely the condition under which it feels safe to drop.

---

**I-08 — Every artefact carries full `contents`; materialise is pure IO; one renderer feeds both paths.**

`build-plan-v1.schema.json:5` — *"Every artefact carries its full `contents` so the consumer's materialise step stays pure IO -- the plan and the Orchestrator's own on-disk materialise step are written from the same `_shared_artefacts`/`_slice_config_artefact` helpers and MUST agree byte-for-byte."*

**Caveat:** the "cannot drift by construction" claim in `buildplan.py:276-280` names a step ADR-0020 Phase 4 deleted (`orchestrator.py:5-8` says so in the same package). The guarantee now rests entirely on the goldens.
**Port rule:** one renderer, both outputs. "Two implementations that agree today" is exactly what this wording forbids.

---

**I-09 — `planPathMode: "tokened"`, and `boardYaml` is deliberately NOT tokenized.**

`buildplan.py:483-499` verbatim: *"`boardYaml` is deliberately NOT tokenized (kept repo-relative as-passed) -- it is the anchor both this plan's own comparator and tan use to locate PROJECT_ROOT in the first place."* Tokens are `${SDK_ROOT}` / `${PROJECT_ROOT}` / `${PYTHON}`; enum is closed to `["tokened"]`. A path under neither root raises `UnrootedPathError` from the single tokenizer (`orchestrator.py:167-173`) and is caught into a `command-unrooted` / `appdir-unrooted` warning (`buildplan.py:344-362`, `:427-440`); `PROJECT_ROOT` is tried before `SDK_ROOT`.

**ENFORCED (single raise-site) + BYTE-PINNED.**
**Port rule:** applying the tokenizing rule "consistently" to `boardYaml` destroys tan's root resolution. Keep `PROJECT_ROOT`-first ordering — reversing it normalises identically under seam-1 (`tests/parity/README.md:127-137`) and is still wrong; only the emit snapshots catch it.

---

**I-10 — `envAppendPath` join separators are PER-KEY, and values are appended with de-dup.**

`buildplan.py:466-478` and the schema: *"skipping any value already present (de-dup first, matching `_alp_common.env_with_sdk` / `_workspace.subprocess_env`) ... `EXTRA_ZEPHYR_MODULES` is a CMake list -- Zephyr's `zephyr_module.py` splits it on `;` on every platform, never an OS path list -- while `PYTHONPATH` is a real OS-native path list joined with `os.pathsep`. Plan wins / CLI fills gaps."*

Three reference implementations exist: `tan-cli/crates/tan-core/src/plan_exec.rs:20-42` (`apply_env_append`, regression test `plan_exec.rs:221 apply_env_append_extra_zephyr_modules_always_uses_semicolon` asserting `"/a;/b"`), `scripts/alp_cli/_workspace.py:66-91`, `scripts/west_commands/_alp_common.py:74-97`.

**ENFORCED ONLY BY `cargo test`.** Nothing on the alp-sdk side tests the join.
**Port rule:** copy `plan_exec.rs:20-42`, not `execute/env.rs` (that is the ZEPHYR_BASE gap-filler, a different job). Reflexive `os.pathsep` for `EXTRA_ZEPHYR_MODULES` breaks Zephyr module resolution on Linux. Blind append double-registers the SDK as a module on a second invocation.

---

**I-11 — A slice is never dropped. `command: null` + a matching `warnings[]` entry, and `executionPolicy` is data the executor reads.**

`_EXECUTION_POLICY = {"unknownBackend": "fail", "missingTool": "skip", "nullCommand": "skip"}` (`buildplan.py:39-43`). Deliberately **optional** at the schema root — verified: top-level `required` is `['schemaVersion','generatedBy','boardYaml','sku','buildRoot','slices','sharedArtefacts','warnings']`, no `executionPolicy`, no `planPathMode`. ADR-0020 Amendment 3 records that making them required at unchanged `schemaVersion` was a breaking change, reverted rather than bumped: strict producer / tolerant consumer.

Warning codes today: `no-command`, `yocto-recipe-missing`, `appdir-unrooted`, `command-unrooted`, `board-tree-missing`. Schema: *"New codes may be added without a schemaVersion bump -- consumers must not treat this as a closed enum."*

**ENFORCED (schema shape + goldens); the four behaviours are cargo-only** (`tan-cli/crates/tan-cli/src/commands/build/execute/mod.rs`, tests at `:1135, :1267, :1346, :1400`).
**Port rule:** always emit, never require. Keep `executionPolicy` as data — do not inline it as executor branches. Do **not** type `warnings[].code` as a closed Python `Enum`.

---

**I-12 — `slices[].backend` is a closed three-value enum, but tan's consumer is deliberately permissive.**

Schema enum `["zephyr","yocto","baremetal"]`; *"`off` cores never reach the plan, so this enum excludes it."* Consumer side, `tan-core/src/build_plan.rs:34-38`: *"`Unknown` is a deliberate catch-all: this used to be a closed 3-variant enum, so ONE slice naming a backend the CLI didn't know ... failed `serde_json::from_str` for the WHOLE plan document, before `executionPolicy.unknownBackend` could ever be consulted per-slice."*

**Port rule:** parse permissively, decide via `executionPolicy`.

---

**I-13 — IPC endpoint IDs are FNV-1a over the UTF-8 channel name.**

`carveout.py:28-34` — offset basis `0x811c9dc5`, prime `0x01000193`, 32-bit mask. Applied at `:259-269`: `low = h & 0x0FF`, `src_ept = 0x400 | low`, `dst_ept = src_ept + 1`. Emitted at `headers.py:82-83`.

**BYTE-PINNED ONLY** — `rpmsg-v2n.build-plan.snap` and `hetero-offload.build-plan.snap` carry the literal `ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT    0x000004e6u`. The unit test (`tests/scripts/test_orchestrate_memory.py:120-122`) deliberately pins only the masking, not the digits.
**Port rule:** copy verbatim. FNV-1 vs FNV-1a, 64-bit, or a different encoding silently changes endpoint IDs and desyncs already-flashed firmware.

---

**I-14 — Carve-outs allocate TOP-DOWN; storage partitions allocate BOTTOM-UP. Do not unify.**

- Carve-out: `_PAGE = 4096` (`memregion.py:18`); `top = _align_down(base + size_bytes, _PAGE)` (`carveout.py:181-184`); size rounded **up** to a page (`:235-236`); `new_top = region_top - carve_size_aligned` (`:247`). Entries sorted alphabetically by name first (`:187-188`). Region ranked by `(0 if cacheable_match else 1, size_b)` with `size_b = _region_size_bytes(r) or 1 << 62` — smallest first, unsized last (`:218-225`), pre-filtered to regions whose `accessible_from` covers **every** endpoint (`:205-208`). Default preference is non-cacheable (`:199`).
- Partition: `sorted(by_device.keys())` (`partition.py:203`), entries name-sorted within a device (`:224-225`), size page-rounded (`:229`), high-water bumped only for auto-allocated entries (`:267-271`).

**ENFORCED (unit) + BYTE-PINNED.**
**Port rules with teeth:** `ResolvedPartition.base_kib` is an **offset within the device**, not a physical address (`partition.py:70-74`). `capacity_mbit` is **megabits** → `int(cap) * 1024 * 1024 // 8` (`partition.py:117-118`) while `size_mib`/`size_kib` are bytes with no `/8` (`memregion.py:21-36`). An explicit `ipc[].address:` is **only** page-alignment-checked (`carveout.py:239-245`) — no range check, no sibling-overlap check, no high-water update; storage's overlap loop at `partition.py:253-264` has no carve-out twin. Reproduce the hole deliberately or you change addresses and break the goldens. Region-rank ties break on `memory_map:` declaration order via Python's stable sort.

---

**I-15 — Incomplete metadata produces `status: blocked` + `reason:`, never an exception.**

`carveout.py:57-74` and `partition.py:133-148`. Verified live: `rpmsg-aen` → `memory_map.base is unset for region 'sram0' in SoM E1M-AEN801`; `rpmsg-imx93` → `SoM E1M-NX9101 mailbox controller is TBD`.

**Resolvability is a SoC-level fact, not a SKU one** (`alp_project_loader.py:458-460` returns the SoC JSON's `memory_regions` verbatim). Of 11 presets: **4 resolve** (`E1M-V2N101/102`, `E1M-V2M101/102`, all `renesas:rzv2n:n44`), 6 AEN resolve regions with zero bases, `E1M-NX9101` resolves none.

**ENFORCED (unit + goldens).**
**Port rule:** blocked-not-fatal. A port that raises on TBD metadata turns every AEN and iMX93 project into a hard failure. Keep the reason substrings — tests assert on `'TBD'`, `'HW-mapped'`, and the SKU.

---

**I-16 — A blocked channel still emits all six macros as zero stubs, and the gap is a COMMENT, never a `#warning`.**

`headers.py:63-67` — *"Surface the gap as a structured comment block rather than a `#warning` (the slice builds under -Werror=cpp and an actual warning would trip it)."* Stubs at `:71-76`.

**BYTE-PINNED.** Note: the neighbouring comments at `headers.py:56-58` and `:113-116` still describe the *old* `#warning`/`#error` behaviour and are stale — as is `docs/heterogeneous-builds.md:241-243`.
**Port rule:** omit the stubs and every rpmsg consumer fails with `<macro> undeclared`.

---

**I-17 — The emit surfaces are byte-golden. 35 files, 11 of 20 modes.**

`scripts/check_emit_snapshots.py:4-13` — *"A refactor must change *shape*, never *behaviour* ... This gate pins them."* Run at `pr-metadata-validate.yml:269-270`. Verified: 35 goldens, 6 `*.build-plan.snap`. Normalisation is exactly three values — `<SDK_ROOT>`, `<PYTHON_EXECUTABLE>`, `sdkCommit` → `<SDK_COMMIT>`; `sdkVersion` is *"deliberately left real"* (`check_emit_snapshots.py:169-171`). A non-zero emit is itself a failure (`:216-218`).

**CI, non-blocking.** Not covered: `cmake-args`, `yocto-conf`, `zephyr-board`, `ipc-contract-h`, `dts-reservations`, `dts-partitions`, `storage-mounts-c`, `tfm-sysbuild-conf`, `kconfig`.
**Port rules:** no timestamps, no PIDs, no host paths outside the two tokens, no dict-iteration-order dependence; token tails use forward slashes even on Windows. Two coupled traps: (a) `scripts/bump_version.py` regenerates `alp.lock` (`:227-237`) and the ABI snapshot (`:209-224`) but **not** the emit snapshots, so every release bump hand-breaks all six build-plan goldens; (b) seam-1 *drops* `sdkVersion` (`seam1_field_diff.py:266`) — the two gates disagree on purpose.

---

**I-18 — `west build` is emitted with NO `-d`, and `artifacts` does not account for the resulting nesting.**

`orchestrator.py:209-214` — *"west's default output is <cwd>/build (a subdirectory of the command's cwd = buildDir), so the tree lands at <buildDir>/build/; the consumer (tan) reconciles that nested layout ... Adding `-d <buildDir>` here would double-nest."* But `_slice_artifacts` reports `<buildDir>/zephyr/zephyr.elf` (`buildplan.py:159-168`).

**BYTE-PINNED (the absence of the flag).** The reconciliation is consumer-side and untested here.
**Port rule:** resolve the off-by-one-directory in the executor. Do not "fix" either half in isolation.

---

**I-19 — A `--sysbuild` slice gets NO `-DEXTRA_CONF_FILE`, and `SB_CONF_FILE` is a layered `;`-list.**

`orchestrator.py:309-318` — *"a bare -DEXTRA_CONF_FILE there lands on the SYSBUILD image, not the default application image (sysbuild scopes per-image as -D<image>_VAR), so it would NOT reach the app -- silently dropping the per-core alp.conf on boot:/OTA projects."* Guarded by `if not is_sysbuild:` at `:319`. Layering at `:276-300`: family base **first**, customer overlay **second**, later files win. Paths must be absolute and forward-slashed (CMake's `cmake_path()` only recognises `/`).

**ENFORCED**: `tests/parity/seam1_field_diff.py:186-198 _strip_863_extra_conf_file_arg` is scoped to non-sysbuild slices *on purpose* — *"stripping the arg unconditionally from EVERY slice, sysbuild included, would silently hide exactly that regression."* The vendored twin in tan-cli (`:191-203`) is byte-identical on that logic; I diffed it.
**Also:** `--sysbuild` is **project-wide, not per-slice** (`orchestrator.py:249` reads the project) and the overlay lives at the shared `build_root` (`buildplan.py:98`). Verified in `iot-fleet-ota.build-plan.snap`: both zephyr slices get identical `-DSB_CONF_FILE`, *including* `m55_he` whose appDir is `${SDK_ROOT}/firmware/alp-stock-shim`. One customer `boot:` block forces MCUboot onto the SDK's stock shim on an idle peer. There is no per-core boot policy.

---

**I-20 — Shared artefacts must be on disk before ANY slice runs. The plan does not say so.**

`buildplan.py:282-284` — *"No `inputHash` ... and no `sequential` (parallelism policy belongs to the consumer's scheduler)."* The only enforcement is at bitbake runtime: `meta-alp-sdk/recipes-core/alp-system/alp-dts-reservations_0.6.bb:52-57` `bb.fatal("alp-dts-reservations: expected the orchestrator's system-manifest.yaml at '%s'. Build with \`tan build\` first...")` and `:59-66` requiring `dirname(manifest)/generated/dts-reservations.dtsi`.

**Worse:** the recipe's default is `ALP_SYSTEM_MANIFEST_PATH ??= "${TOPDIR}/../alp-sdk/build/system-manifest.yaml"` (`:43`) — the **SDK checkout**, not the project. The planner writes under the *project's* build root. The plan carries no field for it (`buildplan.py:465` — `"env": {"ALP_SDK_ROOT": "${SDK_ROOT}"}` is the whole block).

**DOCUMENTED-ONLY + runtime-fatal on infrastructure no public CI runs** (`pr-bitbake.yml` dispatches to a private runner).
**Port rule:** materialise all `sharedArtefacts` and write `system-manifest.yaml` before dispatching any slice; slices may then run in parallel. Set `ALP_SYSTEM_MANIFEST_PATH` on the bitbake command line from knowledge the plan does not give you.

---

**I-21 — The JSON envelope is the API, and every consumer-matched string fails open.**

`tan-cli/contract/README.md:4-13` pins **five** things: the envelope `{command, ok, exitCode, project, data, issues}`; the exit ladder **0 success, 1 runtime, 2 validation, 3 write, 4 doctor, 5 internal**; the frozen issue codes matched with `===`; the `data` field names read with `?? []`; and **`tan --version`'s first stdout line, `tan MAJOR.MINOR.PATCH`**. `:33-37` — *"The extension does not error, does not log and does not warn — it silently skips the check or renders stale data, with CI green on both sides."*

**ENFORCED by `cargo test`** — `crates/tan-cli/tests/contract.rs`, **15** fixtures (verified: `contract/envelopes/` has 15 dirs), plus source-literal assertions `frozen_issue_codes`, `every_emitted_issue_code_is_registered`, `doctor_build_data_keys_the_extension_reads`, `version_first_line_matches_contract` (`:314`), `doctor_build_and_build_fix_stay_accepted_cli_arguments` (`:759`). Fixtures spawn the **real compiled binary** via `CARGO_BIN_EXE_tan`, deliberately (*"that also exercises the exact argv-parsing + stdout-framing path the extension actually shells out to"*).

**Issue-code registry, verified counts:** `contract/issue-codes.json` holds **68** entries — 62 `reserved`, **5 `frozen`** (`bootstrap.yocto-host`, `bootstrap.prerequisites-missing`, `presets.sdk-root-unresolved`, `bootstrap.python-not-runnable`, `bootstrap.python-too-old`), **1 `retired`** (`bootstrap.windows-unsupported`, spelling permanently reserved). The master plan's "27 of them" is the count of entries carrying `consumerEffect` — also verified as 27. **The plan is stale on this number; do not repeat it.**

**Port rules:** reproduce the envelope key set, the ladder, and every frozen spelling byte-for-byte; carry a Python golden-diff suite spawning a real subprocess **before** `contract.rs` is retired. Serialization details Python gets wrong by default: `sdk` is `skip_serializing_if = "Option::is_none"` (absent, never `null`) while `Project.root`/`boardYaml` serialize as explicit `null`; `schemaVersion` is the **string** `"1"`; `ok` is derived (`envelope.rs:79 ok: exit_code == 0`); unresolved scalars are `""` not `null` (`init-invalid-template/expected.json`). Key **order** is deliberately not covered by these goldens (`contract/README.md:160-168`) — pin it in the owning module if it matters.

---

**I-22 — `tan --version`'s first stdout line must be exactly `tan MAJOR.MINOR.PATCH`.**

`contract.rs:305-313` records the consumer: `alp-sdk-vscode/src/alpCli/service.ts` matches `/^tan \d+\.\d+\.\d+/`, `parseTanVersion` **keeps** any suffix, and `cliSkew` implements SemVer §11 so `0.4.0-rc1` sorts strictly below `0.4.0`.

**ENFORCED (cargo test) — dies with Rust.**
**Port rule:** argparse's `--version` prints `<prog> <version>` and `prog` defaults to the script name → `tan.py 0.4.1`. Any deprecation warning, venv notice or import-time print on stdout also breaks it. Failure is total and silent: every version-gated extension feature turns off with no error anywhere. `scripts/alp_cli/doctor.py:365-374` also shells `tan --version` and parses `(major, minor)` off stdout+stderr.

---

**I-23 — The interactive/TTY rule: prompt only when no `--non-interactive` / `--ci` / `--format json` AND both stdin and stderr are real terminals.**

`tan-cli/crates/tan-cli/src/cli.rs:64-79`, with the scar recorded at `:69-74` — *"ONE home for the rule, because it was three: #198 added the terminal term to `init` alone, `bootstrap` had grown its own copy in #185, and `scaffold` had neither and still hung."* And `:118-124` — *"without it `tan init` rendered an inquire prompt to a terminal that was not there and then blocked — no timeout, no diagnostic, no exit. From the caller's side that is indistinguishable from a slow operation."* A missing terminal is non-interactive, **not** an error. `interactive_mode` was extracted as a pure predicate specifically so a dropped term is unit-testable (`cli.rs:137`, tests at `:801`).

**ENFORCED (cargo unit tests) — dies with Rust. Nothing in alp-sdk gates it.**
**Port rule:** Python `input()` / `click.prompt` / `inquirer` inherit none of this. The `--format json ⇒ non-interactive` implication is load-bearing: the extension drives every command with `--format json`. Failure mode is a CI job that hangs until the runner times out.

---

**I-24 — The Python floor is 3.10, pinned in three independent places, reconciled by nothing.**

`pyproject.toml:9 requires-python = ">=3.10"`; `metadata/bootstrap.json prerequisites.pythonMinVersion = "3.10"`; `tan-cli/crates/tan-cli/src/util.rs:115 pub const MIN_PYTHON: (u32,u32) = (3,10);` with the refusal at `generate.rs:233-236` (*"the SDK scripts use `@dataclass(slots=True)`, Python 3.10+"*). The CI/dev pin is `.python-version` = `3.12`, consumed via `python-version-file:`. Frozen code `bootstrap.python-too-old` fires below the floor.

**PARTIALLY ENFORCED**: `check_bootstrap_manifest.py` cross-checks the manifest floor against both bootstrap scripts. Nothing tests SDK Python on 3.10; tan-cli has an explicit `msrv` job for the Rust analogue (`ci.yml:62-83`) and alp-sdk has no Python equivalent.
**Port rule:** run on 3.10, not just 3.12, or the floor and `bootstrap.python-too-old` both become lies. 3.12-only syntax passes CI and fails the maintainer's own Windows host (3.11.3).

---

**I-25 — The release contract is eight raw uncompressed per-triple binaries.**

`tan-cli/docs/release-contract.md:75-80` (raw, no `.zip`/`.tar.gz`), `:18-21` (tag minus `v` must equal the crate version; the `verify-version` job fails the release otherwise), `:189-191` (build-provenance attestation on all eight plus `checksums.txt` and `envelope-contract.json`). Both Linux pairs ship: `x86_64`/`aarch64-unknown-linux-gnu` (cargo-zigbuild, pinned `-gnu.2.31`, measured floor GLIBC_2.30) **and** `-musl`. `release.yml` calls `ci.yml` via `workflow_call` — *"a tag cannot publish binaries the gates never ran against"* (`ci.yml:10-13`).

Three in-repo consumers of the asset names: `install.sh`, `install.ps1`, `npm-shim/postinstall.js:34-39,54` (`TAG = \`v${pkg.version}\``, gated at `release.yml:75-88` after npm-shim went six releases stale).

**ENFORCED (release workflow).**
**Port rule:** highest-risk item. PyInstaller cannot cross-compile, has no glibc-floor mechanism, and cannot produce musl at all. Either ship under the identical eight names or coordinate a breaking change with `alp-sdk-vscode`'s `releaseAssetForTarget` **and** `SUPPORTED_CLI_VERSION`. Also: crates.io (`alp-tan-cli`) and npm (`@alplabai/tan`) publishes become meaningless; `release.yml:373-386` now **fails closed** on a missing token because v0.4.0 shipped advertising `cargo install alp-tan-cli` against a crate that did not exist (#151).

---

**I-26 — `metadata/**` stays in alp-sdk. Generators move; facts never do.**

ADR-0017 `docs/adr/0017-alp-sdk-over-the-vendor-sdk.md:34-35`. Port binding at `docs/superpowers/specs/2026-07-29-tan-python-executor-mvp-design.md:552-568` — *"**`metadata/**` stays in alp-sdk. It does not move.** ... What relocates into `tan` is the **generators**, never the facts."*

**PARTIALLY ENFORCED** — the generated-from-metadata half is gated (`pr-generated-files.yml` regenerates and diffs `soc_caps.h`, `error-catalog.json`, `catalog.json`, the support/portability matrices, board headers, pinmux). The ADR-0017 **tier ladder itself is not gated**: `grep 'Tier 1.5'` across `src/ include/ scripts/` hits exactly one file.
**Port rule:** tan must never learn a hardware fact — no SKU, address, I²C address, pin name, or vendor branch. There is no gate to catch the first one. Counter-example already in-tree: `models.py:315-327` bakes `addr_7bit: 0x50` for the on-module EEPROM with no metadata source.

---

**I-27 — Every generated repo-tree artefact ends with exactly one `\n`, written with `newline=""`.**

`kconfig.py:1453`, `buildplan.py:510` (`json.dumps(plan, indent=2) + "\n"`), `manifest.py:52` (`yaml.safe_dump(out, sort_keys=False, ...)` — **insertion order is the wire order**), `topology.py:131`.

Gate: `scripts/check_write_text_newline.py`, run at `pr-metadata-validate.yml:346-347`. Docstring: *"`Path.write_text()` translates every '\n' to os.linesep on write. On a Windows host that silently rewrites the whole file to CRLF; .gitattributes normalizes it back to LF on `git add`, so the bug never reaches a commit and never reds CI."* Scope is hardcoded `_SCAN_SUBDIRS = ("scripts", "firmware")` (`:72`) and it matches only `.write_text()` — `open(path, "w")` is **not checked**.

**The read side has no gate at all, and a live tripwire already ships:** `metadata/catalog.json` contains 53 non-ASCII bytes, the first at offset 5404 (`b'> \xe2\x9a\xa0\xef\xb8\x8f **\`[UNTES'` — a ⚠️). `scripts/check_e1m_pinout.py:87, :95, :122, :147` all call `path.read_text()` bare. On a cp1252 Windows host that raises `UnicodeDecodeError`; on ubuntu CI it passes. The maintainer's host is Windows 11; the one Windows CI leg is `continue-on-error: true`.

**Port rule:** every write passes `newline=""`, every read passes `encoding="utf-8"`. A tan-side writer is outside the gate's scan roots **by construction**.

---

**I-28 — `sdkCommit` is documented as informational and enforced as a hard refusal. The two disagree.**

Schema `build-plan-v1.schema.json:24` — *"Informational provenance, not part of the locked consumer contract."* Consumer: `tan-cli/crates/tan-cli/src/commands/build/token_substitution.rs:151` emits `build.sdk-commit-mismatch` — *"plan was emitted from alp-sdk commit `{plan_commit}`, but the resolved SDK checkout is at `{resolved_commit}` — building against a different SDK checkout than the plan was captured from can silently produce the wrong image"* (the *two-SDK split-brain guard*, comment at `:142-145`; test at `:643`).

**ENFORCED cargo-only. Nothing gates the contradiction.**
**Port rule:** a porter reading the authoritative schema correctly concludes it is informational and drops the guard — and starts building images against a different checkout than the plan was emitted from. Keep the guard; fix the schema description in the same change.

---

**I-29 — Token substitution has four distinct hard refusals, all cargo-only.**

`token_substitution.rs`: (1) `:126-136` — *"A tokened plan NEEDS a real ${SDK_ROOT} value: degrading an unresolved sdk_root to `""` would substitute `${SDK_ROOT}/scripts` into the bare relative path `/scripts`, sailing straight past the leftover-token guard (there's no token left to catch) — refuse instead."* (2) `:95-114` PROJECT_ROOT-vs-exec-base divergence guard. (3) `:180-188` `build.plan-token-unresolved` — only `${SDK_ROOT}`, `${PROJECT_ROOT}`, `${PYTHON}`, `${TOOLCHAIN_ROOT}` are known. (4) `:199-203` `build.toolchain-root-unresolved` — *"tan refuses rather than substituting an empty path, which would silently build against the host root."*

**Port rule:** a naive `str.replace()` reintroduces all four as silent wrong-path builds. `${TOOLCHAIN_ROOT}` is resolved **lazily** (`:164-172`) so today's SDK plans, which never name it, still build on a host with no detectable toolchain.

---

**I-30 — `ALP_FLASH_FORCE=1` is an undocumented hardware-write safety gate with a distinct non-`ok` status.**

`tan-cli/crates/tan-cli/src/commands/flash/mod.rs:174` `let force_confirm = std::env::var("ALP_FLASH_FORCE").as_deref() == Ok("1");`. `:466-480` — *"This used to report byte-identical to a real write (`status:\"ok\"`), with the reason thrown away, so a `--format json` consumer could not tell \"nothing was written\" from \"programmed the device\". Keep rc 0 ... but give it a distinct status"* — entry status `"planned"` plus a warning Issue, message `"would run {display} -- NOT written: flash_args.confirm is false (set ALP_FLASH_FORCE=1 or flash_args.confirm: true to actually flash)"`.

**Gated by nothing.** `grep -rn ALP_FLASH_FORCE` over alp-sdk docs returns zero; `docs/cli.md`'s Environment table lists only `ALP_SDK_ROOT` and `ZEPHYR_BASE`.
**Port rule — two failures, both silent, both hardware:** drop the gate and tan writes a customer's eMMC/xSPI unasked; keep the gate but collapse `"planned"` back into `"ok"` and a JSON consumer again cannot distinguish "nothing written" from "device programmed".

---

**I-31 — `scripts/alp_project.py` is tan's hardcoded SDK-root marker.**

`tan-core/src/project.rs:61-63` — *"`scripts/alp_project.py` is the canonical marker for an ALP SDK root."* Hardcoded again at `loader.rs:255`, `generate.rs:249`, `clean.rs:11-12`, `doctor.rs:2190-2196`, `build_readiness.rs:821`.

**ENFORCED cargo-only**, plus the deliberate no-`--sdk-root` step in tan-cli's `first-blink` job (`parity.yml:738-746`, regression test for #218: discovery once checked root, siblings and ancestors but never a **child**; *"Passing the flag here again would hide the bug it was found by."*).

**Port rule:** renaming or relocating `alp_project.py` — a natural move when tan becomes the planner — breaks every `--sdk-root` resolution, `tan doctor`, `tan clean`, `tan generate`, and degrades `check_emit_registry.py` to finding nothing. SDK-root search order must cover root, siblings, ancestors **and children**. Also: `--sdk-root` is terminal and returned as-is *even when invalid* (`sdk.rs:319-322`) so a bad path fails loudly instead of silently falling through to a lower tier; the Pythonic `if not isdir(p): continue` inverts this.

---

**I-32 — `tan init`/`tan scaffold` are SDK-free: they read a vendored `--emit scaffold` tree baked into the binary.**

`tan-cli/tests/parity/scaffold_byte_parity.py:6-10`; `crates/tan-core/src/wizard/vendored/MANIFEST.md:3-8` — *"This tree is `alp-sdk --emit scaffold` output, captured byte-for-byte (LF, no retouching) and checked in so `tan init`/`tan scaffold` can read it without ever shelling the SDK"* / *"Re-vendored by re-running the emit, not by editing these files."* Vendor point pinned to `cdfe13684e362c75f6df2b190ec1c3e736c48731`, the same literal as `parity.yml`'s `PINNED_SDK_TAG`.

**Port rule:** "simplifying" by shelling the SDK gives `tan init` an alp-sdk-checkout dependency it deliberately does not have — and the byte-parity script keeps passing, because it only compares the vendored tree to upstream. The LF capture is a live CRLF trap for a Windows re-vendor (see I-27).

---

**I-33 — `metadata/emit-registry-v1.json` is the public frontend emit contract, and 15 of its 20 modes are NOT owned by `alp_orchestrate`.**

Verified: 20 modes, `Counter({'alp_project': 15, 'alp_orchestrate': 5})`. Each mode carries `owner.{cli,module,function}`, `scope`, `options.{input,output,core,build_root}`, `compatible.os`, and `output.{media,schema_id,path,consumer_hint}` — e.g. `zephyr-conf` → `owner.module = "scripts/alp_orchestrate/kconfig.py"`, `owner.function = "_slice_alp_conf"`, `output.path = "build/<core_id>-zephyr/alp.conf"`.

`scripts/check_emit_registry.py:38-39` parses the real `choices=[...]` by `ast` out of **two hardcoded paths**: `REPO/"scripts"/"alp_project.py"` and `REPO/"scripts"/"alp_orchestrate"/"cli.py"`. Enforced at `tests/scripts/test_check_emit_registry.py:46 test_real_emit_modes_matches_registry_exactly` (asserts `code_modes == registry_modes`) and `:52 test_every_mode_field_is_grounded_in_code_paths`, both in the CI pytest sweep.

**CI (via pytest), non-blocking.**
**Port rule:** this file is the **relocation manifest** — it names every owner the port must move. Move either CLI and the AST parse `SystemExit`s. Per-mode option contracts are NOT uniform and must survive a unified front door: `zephyr-board` requires `--output` and `--core`; `scaffold` requires `--template` and `--sku` (output optional); the five `alp_orchestrate`-owned modes are stdout-only; `--build-root` affects `build-plan` alone; ten modes declare `core: "ignored"` rather than `"optional"`. A mode declaring an incompatible `os` is a **hard error, not a skip** (#605).

There are **four** emit front doors, not two — `scripts/alp_cli/emit.py:1-18`: *"Delegation, not duplication: every mode is emitted by the ONE emitter ... So `alp emit`, `west alp-emit`, and `alp_project.py` can never emit differently."*

---

**I-34 — `--emit kconfig` is the only non-hermetic emit and the only registry gate inside a merge-blocking context.**

`metadata/quality-tasks-v1.json:215` — *"needs a bootstrapped Zephyr workspace (ZEPHYR_BASE), unlike every other hermetic gate in this registry."* Run at `pr-twister.yml:238-246` under `if: matrix.shard == 1`, so it feeds `twister-shard 1/4` (required on dev) and the `twister` aggregator (required on main). `scripts/check_emit_kconfig_contract.py:5-30` asserts valid JSON, `schemaVersion == 1`, non-empty `symbols`, and key-set conformance against `tests/fixtures/kconfig-contract/emit-kconfig.golden.json` — *"tan-cli's `parse_kconfig` and alp-sdk-vscode's `kconfigSymbolsFromEnvelope` both test against the same file, so a key rename here needs a `schemaVersion` bump + coordinated updates there."*

**BLOCKING.** This is the single gate whose failure actually stops a merge.
**Port rule:** exit **2** (not 1) on an unbootstrapped workspace; never emit an empty symbol list. Anything changing ZEPHYR_BASE / EXTRA_ZEPHYR_MODULES derivation breaks inside the required context.

---

**I-35 — The aggregator job name IS the merge gate on `main`.**

`.github/workflows/pr-twister.yml:271-276` — *"Its name MUST stay `twister · native_sim/native/64` (the repointed required check) regardless of how many shards run."*
**Port rule:** renaming that job silently removes the only merge gate on `main`.

---

### 1.2 IMPORTANT

**I-36 — The `os:` enum is read from the schema at runtime; `CLASS_RUNTIMES` three lines below is not.**
`topology.py:52-56` — *"Derived (not re-typed) so the value-set has exactly one source of truth and cannot drift"* — reading `$defs/core_entry/properties/os/enum` (verified `['zephyr','yocto','baremetal','off']`). But `topology.py:63 CLASS_RUNTIMES = ("yocto", "zephyr")` is hardcoded and consumed at `:79`. The class predicate itself is duplicated **five** times: `topology.py:39/41`, `topology.py:69/71`, `libraries.py:84/86`, `libraries.py:96/98`, `kconfig.py:1048`. **Half-enforced; nothing cross-checks the copies.**

**I-37 — `load_board_yaml` runs a five-stage pipeline whose ORDER is load-bearing.**
`loader.py:665-708`: schema validate **first**, `_normalize_libraries` **second** (it injects a `libraries` key into `cores.<id>` that `core_entry`'s `additionalProperties:false` forbids), `_validate_consistency` **last**. **DOCUMENTED-ONLY for the ordering.** Validating after normalisation — a natural refactor — makes every core-scoped-library project fail with an `additionalProperties` error.

**I-38 — Nine further loader hard errors, each raised on every load, gated only by unit tests.**
(1) board preset `hosts_som_families:` must list the SoM's `family:` (`loader.py:125-140`); (2) `preset:` vs inline is a schema `oneOf`, and `name:` is conditionally *required* (with `populated`/`e1m_routes`) and conditionally *forbidden* (with `preset:`) — `board.schema.json:39-60`; (3) `_resolve_topology_for_core` is a **shallow** per-key merge, preset first (`loader.py:168-172` — `merged = dict(som_topology[core_id]); merged.update(project_cores[core_id])`), so a declared core keeps the preset's `board`/`toolchain`/`machine`/`hw_console`; (4) top-level `pins:` cross-check against `e1m_routes:` over the fixed section list `("gpio","buses","pwm","adc","dac","i2s","can","qenc")`, one pad may carry several macros (`:399-438`); (5) `storage[].flash_device` known + `storage[].name` unique (`:476-496`); (6) PSA `its_storage`/`ps_storage` must resolve, `attestation_root: optiga_trust_m` requires real OPTIGA (`:501-582`); (7) `_silicon_to_soc_path` requires a triple-colon ref (`:43-50`); (8) `raw: true` → `fs: raw`, `fs` defaults `raw` (`:461-465`); (9) `iot.tls: true` requires `mbedtls` or `bearssl` (`validate.py:201-214`).

**I-39 — `extra_libraries:` obeys four rules the schema cannot express.**
Exactly one of `kconfig:`/`profile:` — `validate.py:118-128` uses `if has_kc == has_pf:` which rejects both-set **and** neither-set in one comparison (`if has_kc and has_pf` loses half of it); names globally unique across all cores; no collision with the curated set; a `profile:` must resolve to a real file.

**I-40 — `_CURATED_LIBRARIES` is 23 names, live, and its stated single source no longer exists.**
`validate.py:26-39` mirrors *"metadata/schemas/board.schema.json `cores.<id>.libraries.items.enum`"* — that property does not exist in `core_entry` (verified). `scripts/check_library_registry.py:30-40 _curated_tokens()` therefore returns the empty set and the coverage half of the gate passes **vacuously**. Note the spelling mismatch: `cmsis_dsp`/`tflite_micro` (underscores) here vs `cmsis-dsp`/`tflite-micro` (hyphens) in the manifests, so the collision check only fires on the underscore forms. **Copy verbatim; know it is unmaintained.**

**I-41 — `libraries.py` raises on seven conditions, one with a live unit bug.**
`resolve_selection` / `_check_requires` (`libraries.py:107-192`) reject an unknown name, missing `requires.capabilities`, `min_ram_kib` vs `soc_ram_kb`, `min_flash_kib` vs `soc_flash_mb * 1024`, `core_class`, `os`, and a manifest with no `integration:` for any live OS. **`min_ram_kib` is compared to `soc_ram_kb` with no kb↔KiB conversion**, and every numeric check is skipped silently when the SoC field is `None`. A port that "fixes" the unit newly rejects projects.

**I-42 — Core-scoped `libraries:` validate against the *declared* `cores:`, not the fanned-out set.**
`loader.py:615, 630-634` — scoping a library to a core the topology exposes (and which still becomes a slice) is a hard error if the customer never typed it. Heterogeneous-by-default does **not** extend to libraries.

**I-43 — Chip on/off state resolves with strict three-tier precedence, printed exactly once.**
`kconfig.py:661-683`: on-module = TRUE baseline → board `populated:` overrides → project `chips:` can turn a board-DNI'd chip on **unless** it is also on-module (`if chip in som_chips: continue`). Print-once at `:709-721`. `_resolve_chip_states` is the single source both `_emit_chips` and `_zephyr_iot_kconfig` read (`:355`) — recomputing independently reopens the `WIFI_CC3501E=y`-against-`CHIP_CC3501E=n` contradiction #874 fixed.

**I-44 — `_ON_MODULE_NON_CHIP_FIELDS` excludes by NAME, and there is already a hole.**
`slugs.py:42-56` — *"They are routing annotations, not chip slugs ... emitting them as CHIP_<NAME> trips the Zephyr build with an undefined-symbol warning."* `assembled: optional` devices skipped (`:101-104`). **But** `slugs.py:87 if isinstance(val, str):` means any new *scalar string* field on `on_module:` becomes a bogus `CONFIG_ALP_SDK_CHIP_<NAME>` today; the docstring at `:65-68` claims `hyperram` is excluded when it is not in the frozenset — it is safe only because all six AEN presets declare it as a dict.

**I-45 — Kconfig section ORDER is semantically load-bearing.**
`kconfig.py:1419-1451` fixed sequence; `_emit_inference` may emit `CONFIG_HEAP_MEM_POOL_SIZE=65536` (`:1162`) and `_emit_memory` runs after it and may re-emit from `memory.heap_kib` (`:1186`) — Kconfig takes the later value. Reordering "for readability" changes which value wins. Enforced by `check_emit_snapshots` **and** `check_zephyr_conf_parity` (`tests/scripts/test_check_zephyr_conf_parity.py:38`), which additionally hard-fails an **unscoped** (`--core`-less) `--emit zephyr-conf` (`check_zephyr_conf_parity.py:71-76` — *"the cross-core Kconfig leak ADR-0020's addendum retired"*).

**I-46 — `heap_kib` and `stack_kib` have deliberately different zero-semantics.**
`kconfig.py:1181-1186`: `stack_kib`/`isr_stack_kib` emit only when truthy; `heap_kib` emits `is not None` (so `0` emits, disabling the kernel heap). Untested — no example declares a zero. Looks like a bug; is not.

**I-47 — Console backend is auto-selected; an unrecognised value falls through to `none` silently.**
`kconfig.py:223-230` and the alias table `:194-204`. Rationale at `:216-221`: *"emitting `CONFIG_UART_CONSOLE=y` on a core whose board has no serial driver is an \"assigned y but got n\" Kconfig error, fatal to the Zephyr build (issue #717)."* `hw_console` comes only from the SoM preset. `diagnostics:` is **project-wide** — one `console:` value runs through every slice including Yocto.

**I-48 — Ethos-U sizing reads the NPU paired to *this* core and fails loudly rather than `max()`-ing.**
`kconfig.py:1127-1143` — *"add npus[].paired_core so the build sizes the accelerator for this core instead of guessing (see #909)"*; validated against `_valid_accel` (`:1148-1158`). **Both raise bare `ValueError`, not `OrchestratorError`**, so they escape `cli.py:79-81` as a traceback. That traceback IS today's observable behaviour; converting is a behaviour change.

**I-49 — `boot.signing.algorithm: rsa3072` raises at emit time and aborts the whole plan.**
`secure.py:109-116` — no honest sysbuild expression exists. `validate.py:65` *allows* rsa3072 for renesas-rzv2n and nxp-imx9, so this raise is reachable on a schema-valid, consistency-valid project. Fires from inside `_slice_command`. **No test covers it.** Do not soften — silently shipping the wrong RSA key length is a secure-boot failure on hardware.

**I-50 — OTA provider and boot-algorithm validation, with a deliberate unknown-family pass-through.**
`validate.py:160-183`, `:62-68` (`return None` for an unknown family — *"don't block on missing capability data for in-development presets"*), applied at `:190-199`. Defaulting to an empty frozenset blocks every new SoM family.

**I-51 — Consistency rules 4 and 5 are stderr WARNINGS, not errors.**
`validate.py:222-227` (arena > heap) and `:235-240` (sleep_mode with no wakeup_sources). The emit surfaces are therefore **not** stderr-clean; `check_emit_snapshots.py:219` compares stdout only. A harness that promotes warnings to failures breaks green projects.

**I-52 — Per-OS loader rules and their ORDER.**
`validate.py:257-279`: `off` exempt; `zephyr`/`baremetal` require `app:`; `yocto` requires `app:` **or** `image:`; unknown os = hard error. Called at `loader.py:368` **before** the cross-class check at `:369-370` — a missing `app:` is reported before a cross-class OS. Preserve the order or messages change.

**I-53 — Strict mailbox reservation, with a known hole.**
`carveout.py:135-145` blocks every rpmsg entry unless a channel carries `reserved_for: alp_default_rpmsg`. **But** the per-entry lookup (`:51-54`) matches `reserved_for == entry_name` and falls through to `return 0`, so any rpmsg channel not literally *named* `alp_default_rpmsg` still gets mailbox 0 — and the only collision check (`:261-266`) is on the FNV low byte, not the mailbox channel. Two differently-named channels collide on 0 today, silently. Classify as a **partial** guard.

**I-54 — Endpoints must be live cores; `endpoints` is `minItems: 2, uniqueItems: true` with NO maximum.**
`loader.py:388-397` (loader rule §4.5.6, runs before carve-out resolution). `board.schema.json:714-717`. The resolver is endpoint-count-agnostic (`carveout.py:200-208`); `docs/heterogeneous-builds.md:197-198`'s "exactly two" is **DOCUMENTED-ONLY**. Do not assume `len(endpoints) == 2`.

**I-55 — `kind:` is a three-value enum but only `rpmsg` is guarded.**
`board.schema.json:711` `["rpmsg","raw_shmem","mailbox_only"]`. A `mailbox_only` entry still requires `carve_out_kb >= 1`, still gets a real carve-out, and still resolves `mailbox_channel: 0` on a TBD-controller SoM. No example exercises it, so no golden pins it.

**I-56 — Yocto app-only slices need an explicit `recipe:`; two magic tokens behave oppositely.**
`orchestrator.py:329-343` + `buildplan.py:386-393` (`yocto-recipe-missing`). `alp-stock-shim` (`STOCK_SHIM_APP`, `:74`) resolves to an SDK-owned **directory**; `alp-image-edge` (`STOCK_IMAGE_APP`, `:84`) passes through as a bitbake **recipe name**. Do not collapse them.

**I-57 — A Zephyr slice with no board tree is blocked at emit; the check uses the BARE name.**
`orchestrator.py:200-208` (`bare = raw.split()[0].split("/")[0]`, the same normalisation `check_board_target_tree_parity.py` uses), caught at `buildplan.py:363-377`. Metadata gate at `pr-metadata-validate.yml:337-338` with an `_NOT_YET_SUPPORTED` allowlist of 11 declared-but-unbuilt targets. **The full qualified string still goes to `west build -b`.** The system-manifest has no equivalent guard — it reports those 11 as buildable.

**I-58 — `resolve_capabilities` and `resolve_memory_map` precedence, with Python bool traps.**
`alp_project_loader.py:530-542` — SoM overrides SoC, then `unpopulated` forces `0` for counts and `False` for flags, preserving the class via `isinstance(base, int) and not isinstance(base, bool)`. `resolve_memory_map` has **three** levels, and the middle one is undocumented and load-bearing: preset `memory_map:` (`:437-440`) → **SoC JSON `memory_regions` short-circuit** (`:458-460`) → variant-derived (`:465-490`, which never emits a `base`, which is exactly why Alif blocks). Returns `[]` (never raises) when unresolvable. TCM detection by `_<CORE>_` substring, marked non-cacheable and single-core. `memregion.py:21-36` returns `None` for a non-`int` size (the literal `"TBD"`) — note `isinstance(True, int)` is True in Python.

**I-59 — `_zephyr_app_dir` stats the filesystem and can rewrite the app dir.**
`orchestrator.py:373-392` — `app: ./src` emits the example **root** as the `west build` argument if the parent has the `CMakeLists.txt`. This qualifies the plan's own "byte-identical wherever invoked from" claim; a partial checkout or a different host can resolve differently.

**I-60 — The system-manifest carries `helper_mcus`, `carve_outs`, `partitions` and `boot_order`. The build plan carries none of them.**
Verified plan top-level keys: `schemaVersion, planPathMode, generatedBy, sdkVersion, sdkCommit, boardYaml, sku, buildRoot, executionPolicy, slices, sharedArtefacts, warnings`. `helper_mcus` (`manifest.py:60-121`) is the only carrier of `gd32_bridge.bin` / `cc3501e_otp.blob`, described in `som-preset-v1.schema.json:226` as *"NOT heterogeneous-compute peers ... independently flashed via `tan flash`"*. **A tan that drives purely off the plan loses helper-MCU firmware entirely.**

**I-61 — Manifest wire shape details that are byte-pinned.**
`sort_keys=False` → insertion order is wire order: `schema_version, generated_by, hw_info, slices, ipc, helper_mcus, boot_order`, with `storage` appended **last and only when non-empty**. `ResolvedCarveOut.to_manifest_entry` (`models.py:200-213`) emits 8-digit zero-padded lowercase hex **strings** and nests `rpmsg_endpoint_ids: {src, dst}`. `ResolvedPartition.to_manifest_entry` (`:255-273`) emits a **different key set** for blocked vs ok. `emit_system_manifest` appends a trailing reviewer comment when `boot_order` is empty — that comment text is part of the golden. `duration_s` is deliberately excluded (`models.py:120-124`). Note the case split: build-plan and board.yaml use camelCase `schemaVersion`; system-manifest uses snake_case `schema_version` — pinned by `tests/scripts/test_schema_version_negotiation.py`.

**I-62 — `slices[].build_dir` / `output_artefact` / `log_path` / non-`pending` status are DEAD.**
No in-repo caller passes `slices=` to `emit_system_manifest` (the SDK executor was retired). Optional in the schema, so their permanent absence is invisible. **If tan becomes the executor it must populate them** — and then re-check `Slice.to_manifest_entry`'s None-dropping (`models.py:150-151`) and the `duration_s` exclusion, or byte-stability breaks.

**I-63 — alp.lock is a hidden blocking gate with a documented blind spot and a CRLF trap.**
Run at `pr-metadata-validate.yml:352-353` inside the job named `validate · soc-spec-v1` — which is why it surprises people. Digests cover **only** `metadata/schemas/*.schema.json` (top level, non-recursive) and `metadata/**/*.yaml` (`scripts/alp_lock/__init__.py:138-142`) — 23 metadata JSON files (every SoC spec, `emit-registry-v1.json`, `quality-tasks-v1.json`, `bootstrap.json`, `error-catalog.json`) are invisible. Ordering must key on POSIX `.parts`, never `sorted(Path)` (`:118-127` — *"the same tree digested to a different sha on Windows ... re-locking there would have committed a Windows-ordered digest that reds CI for everyone"*). No local/absolute path may be recorded (`:24-31`); `sdk.revision` is provenance and excluded from drift comparison (`:203-210`).

**I-64 — `metadata/bootstrap.json` is a live tan consumer contract, and `_comment` is a REQUIRED key.**
`bootstrap-v1.schema.json:5` — *"tan (Rust, cross-platform) has read the same facts since tan-cli PR #55 ... not merely an INTENDED future consumer."* Required top-level includes `_comment`; `/env/ZEPHYR_TOOLCHAIN_VARIANT` is `const: "zephyr"`. `prerequisites.install.{linux,macos,windows}` carries real winget/apt/brew strings — tan-cli README:233-238: *"The install commands come from the SDK's own `metadata/bootstrap.json` ... not from a table `tan` carries."* `check_bootstrap_manifest.py` has an **orphaned-leaf walk** that inspects only `bootstrap.sh` and `bootstrap.ps1` — a leaf consumed only by tan reports as orphaned. **This file and its schema are being edited on the sibling branch right now; re-read before porting.**

**I-65 — `metadata/toolchains.json` pins the Zephyr SDK; the scanner covers EVERY workflow.**
`check_toolchain_lock.py:26-37` fails any `.github/workflows/*.yml` naming the sdk-ng URL with a version slot that is not `${{ env.ZEPHYR_SDK_VERSION }}`, or with no `ZEPHYR_SDK_SHA256`, or a cache key with a literal `vX.Y.Z`. Host enum is closed to four triples. Runs via the pytest sweep (`test_check_toolchain_lock.py:125`), and is *quoted* in four workflow comments but never as a `run:` step.

**I-66 — Templates map to a canonical existing example; four closed enums; `preview` requires a `note`.**
`template-catalog-v1.schema.json:5` — *"never a copy -- the example IS the template's rendered form."* Four scaffold renders are byte-pinned. Scaffold SKU substitution must drop trailing inline comments with the value, and re-derive E1M→E1M-X pin renames via `board_alias:` (#876).

**I-67 — Diagnostic ranges are ZERO-based (LSP); SARIF is a separate 1-based artefact.**
`diagnostic-v1.schema.json:5` — *"the SDK's internal `Diagnostic.line`/`.col` are 1-based (Rust-style) and are converted on export, never mutated in place ... SARIF `region` is 1-based by spec, the opposite convention, so the two exporters must not share range values."* Consumers MUST reject an unknown `schemaVersion`. **`check_diagnostic_schema.py` has no confirmed CI path** (see the §1 preamble) — an off-by-one here is invisible and surfaces as IDE squiggles on the wrong line. `metadata/error-catalog.json` is **generated** (`gen_error_catalog.py`, diff-gated in `pr-generated-files.yml`), not hand-editable.

**I-68 — tan PARSES alp-sdk's human diagnostic TEXT as a machine interface.**
`tan-core/src/validate.rs:192-196` — a faithful port of the TS `parseValidationIssues`, handling `error[ALP-B*]` blocks with `-->` arrows, indented continuations, legacy `FAIL`/`WARN` lines, and standalone `hint:` lines. The only pin is a hardcoded sample in tan's own unit test (`validate.rs:522`). **alp-sdk has no gate on its own renderer's text shape.** A cosmetic reflow silently empties tan's `issues[]` — validate still exits 2, the customer sees no diagnostics, both repos green.

**I-69 — `--emit build-plan` ignores `--core`, contrary to ADR-0020's own request.**
`cli.py:39-42` — `"Core id to scope a per-core emit mode to (required by --emit kconfig; every other mode ignores it)."` ADR-0020:353-354 asked to *"reject the combination"*. Never implemented. `--core X --emit build-plan` silently emits every core — a live footgun the port inherits unless fixed.

**I-70 — Seam-1 is a SHAPE gate; it deliberately drops artefact CONTENT; the oracle can never be regenerated.**
`tests/parity/seam1_field_diff.py:201-216 _drop_artefact_contents` (path kept, so an artefact appearing/vanishing/moving still fails). Exactly one allowed semantic delta: `slices[*].debug.probe` `"openocd"` → `null` (`:94-95, :306-312`) — which exists because `_slice_flash_recipe` deliberately forces no runner (`orchestrator.py:57-66`: *"not every in-tree board registers an openocd runner (e.g. AEN's board.cmake sets flash-runner: alif_flash), so `west flash --runner openocd` FATAL-errors on those boards"*). The oracle was captured at `97ad481b` and `seam1_field_diff.py:29-37` states *"this is the last frame where that comparison exists"* — every subsequent delta is a hand-reviewed edit in `tests/parity/oracle/ORACLE-PROVENANCE.txt`. The comparator's own 11 unit tests run **first** (`parity-seam1.yml:43-56`), because tan-cli#156 proved a bad tolerance once accepted a slice's `command` vanishing silently.

**I-71 — The cross-repo dispatch exists, is path-locked to seam-1, and fails invisibly.**
`.github/workflows/dispatch-tan-parity.yml` (landed #1013) mints an org App token and calls `gh api repos/alplabai/tan-cli/dispatches -f event_type=alp-sdk-planner-change`. Its path list is byte-identical to `parity-seam1.yml`'s (verified: `scripts/alp_orchestrate/**`, `metadata/**`, `examples/**/board.yaml`, `tests/parity/**`) and that lockstep is **machine-enforced** by `tests/scripts/test_dispatch_paths_match_seam1.py:46` — *"A comment saying 'kept in lockstep' is not a mechanism; this is."* `:82-85` — *"GitHub accepts an unrecognised event_type with a 204, so the failure looks exactly like success from this side."* The receiving block must live on tan-cli's **default** branch (#194: lifetime run count was 0 while it lived only on `dev`).

**I-72 — `tan doctor --build`'s check NAMES are frozen; `--build` itself must never be retired.**
`contract/README.md:110` (`data.checks[].{name,status}`, `data.summary.{pass,warn,fail}`, `data.nextSteps`, the literal check name `workspace`), `:117-119` (plain `tan doctor`'s vocabulary is deliberately NOT frozen). `cli.rs` — *"It must not be retired, or turned into a usage error, because both `alp-sdk-vscode` call sites ... hardcode it as a literal argv entry with no fallback -- `[\"doctor\", \"--build\"]` and `[\"doctor\", \"--build\", \"--fix\"]`."* Gated at `contract.rs:759`. **Three doctors exist** with different marker vocabularies (tan `[+]/[!]/[x]`; `python -m alp_cli doctor` `[PASS]/[WARN]/[FAIL]` with `--strict`).

**I-73 — `bootstrap.yocto-host` carries one spelling at two severities.**
`issue-codes.json:57-64` — *"The consumer ALSO requires severity 'error'. The mixed-board WARNING reuses this same suffix at severity 'warning' and must stay a warning -- promoting it would refuse a board that can bootstrap its Zephyr cores."* **The frozen-code gate checks spelling, not severity** — collapsing the two is a regression nothing catches.

**I-74 — SoM topology parity, and the #995 lesson.**
`check_som_topology_parity.py:88-93` + `pr-metadata-validate.yml:284-298`: an unresolvable `silicon:` key must **fail, not skip** — *"a prior version of this check read the dead `soc_ref`/`soc` keys, which no preset has, and so was inert: every preset resolved to \"\" and hit a `continue`, so the comparison never ran (#995)."* Loader-side subset guard at `loader.py:304-309`.

**I-75 — `carrier-netlist-v1` and `contribution-v1` have no validator under `scripts/`.**
But carrier-netlist **is** schema-validated in the test tree: `tests/scripts/test_emit_composed_route_table.py:301/305` (`test_schema_is_valid_draft202012`, `test_aen_output_validates_against_schema`), in the CI pytest sweep. `contribution-v1` remains unvalidated.

**I-76 — `tan flash`/`tan image` take an optional positional `APP_PATH` (default `.`); `tan build` does not, and there is no `--board` and no `--core` on build.**
`tan-cli/crates/tan-cli/src/cli.rs:397-436` vs `:480-526` (`BuildArgs` = `plan, plan_from, materialise, native, manifest, manifest_from, no_auto_bootstrap, pristine`); `--project` is global and pre-subcommand (`:28-29`). Preserve the asymmetry, including `image`'s explicit-positional-overrides-`--project` precedence.

**I-77 — `tan completion` ships bash/zsh/fish only — no PowerShell.**
`cli.rs:720-724`; scripts committed under `crates/tan-cli/src/commands/completion_scripts/`. A customer-facing deliverable the port must reproduce, and a standing contradiction of the Windows-parity story.

**I-78 — Four-tier SDK resolution surfaced on the wire as `sdk.sourceTier`.**
`tan-core/src/sdk.rs:318-337`: `SdkRootFlag > ProjectPin (<workspace>/.alp/sdk-path) > GlobalDefault (~/.alp/sdk-default) > Discovery > None`. Tier values appear in the goldens (`presets-heterogeneous-som`, `sdk-current-no-sdk`), so a rename fails cargo test. **Two persistent on-disk state files written by `tan sdk switch` are part of the customer contract.**

### 1.3 MINOR (real, low blast radius — listed so nobody re-discovers them as regressions)

- **`generatedBy` points at a file that does not exist.** `buildplan.py:492` emits `"generatedBy": "scripts/alp_orchestrate.py"`; `ls` confirms no such file (it is a package directory). Required by the schema, pinned by all six goldens, four lines from the stale "cannot drift" docstring.
- **`emit_os_topology`'s docstring claims sorted keys the code does not request.** `topology.py:128-131` — rows *are* sorted (`:106`), keys are insertion-ordered. Adding `sort_keys=True` breaks two goldens.
- **`carveout.py:6-8` calls the FNV hash a "region id".** It derives endpoint IDs; `region` is a plain name string (`models.py:184`).
- **Blocked-partition `size_kib` is dead.** `partition.py:144` sets it, but the blocked branch emits neither a `reg` (`headers.py:209-214` emits comments only) nor `size_kib` in the manifest (`models.py:255-262`).
- **`ResolvedCarveOut.cacheable` reporting asymmetry.** Resolved reports the *chosen region's* flag (`carveout.py:280-282`); blocked hardcodes `False` (`:71`).
- **`_sku_path` divergence.** `alp_cli/validator.py:178-181` resolves a SKU by `rglob`; `loader.py:243-247` uses a flat path. A nested SoM YAML validates clean then fails to load. Pick one.
- **Two validation depths.** The loader's `_validate_board` runs `iter_schema_errors` only (`loader.py:80-95`), skipping the xref pass (ALP-B005..B009, **errors**) and the compat pass (ALP-B010, warnings). `alp validate` is materially stronger than the build path.
- **helper_mcus legacy branch emits rows without the schema-required `chip` key** (`manifest.py:112-120` vs `system-manifest-v1.schema.json:87`). Unreachable today because all 11 presets carry `helper_firmware:`. Per the no-legacy-compat rule, drop it — confirm with the maintainer.
- **`build-receipt-v1` carries no wall-clock field by construction.** Adding a timestamp "for debugging" destroys reproducibility and nothing notices.
- **Multicore Linux CMakeLists fall back one directory too high.** `examples/multicore/*/linux/CMakeLists.txt` sets `_alp_gen` to `../../build/generated`, which from `<project>/linux/` resolves outside the project. The only reliable path is `ALP_GENERATED_DIR`, which the plan does not set.

---

## 2. WHAT SILENTLY STOPS BEING CHECKED

Ranked by "green everywhere, broken in the field".

**S-01 — Every relevant workflow is `paths:`-filtered on the current file layout. Relocating the planner turns them all off, silently.**
- `pr-metadata-validate.yml:26-40`: `scripts/alp_orchestrate/**`, `scripts/alp_project.py`, `scripts/alp_project_loader.py`, `scripts/check_emit_snapshots.py`, `tests/scripts/**`, `tests/fixtures/emit-snapshots/**`, `alp.lock`, `metadata/**`.
- `parity-seam1.yml:14-27` and `dispatch-tan-parity.yml:46-50`: `scripts/alp_orchestrate/**`, `metadata/**`, `examples/**/board.yaml`, `tests/parity/**`.
- `check_emit_snapshots.py:41` invokes `[sys.executable, "-m", "alp_orchestrate"]`.

The instant the planner stops living under `scripts/alp_orchestrate/**`, the emit-snapshot gate, the whole `tests/scripts/` pytest sweep (which carries build-plan, system-manifest, emit-registry, template-catalog, bootstrap-manifest, toolchain-lock, zephyr-conf-parity, carrier-netlist), the alp.lock check, seam-1, and the cross-repo dispatch **all stop firing on planner changes while still reporting green**. Nothing detects a path filter that no longer matches the code it guards. **This is the single highest-value item in the document.**

**S-02 — 1098 `#[test]` across 108 files in `crates/` disappear with Rust.** Concentrations: `execute/mod.rs` 29, `plan_tokens.rs` 20, `plan_exec.rs` 16, `build_plan.rs` 15, `execute/manifest.rs` 15, `host_env.rs` 10. Everything in §1 marked *cargo-only* — the envelope, exit codes, frozen codes, `--version`, the TTY rule, `executionPolicy` behaviour, the four token refusals, the sdkCommit guard, per-key env join, `ZEPHYR_BASE` derivation, SDK-root discovery — lives inside that number.

**S-03 — tan-cli's five Python parity scripts SURVIVE the port and keep reporting green while losing their teeth.** `tests/parity/toolchain_lock_parity.py:25-31` states it explicitly: *"What the byte-diff buys, **together with `cargo test`**: `host_env.rs`'s `zephyr_sdk_install_version_matches_the_real_toolchain_lock` asserts `ZEPHYR_SDK_INSTALL_VERSION` equals the vendored fixture's `zephyrSdk.version`."* Same construction in `bootstrap_manifest_parity.py:27-31` and `kconfig_fixture_parity.py:23`. Remove the cargo half and each degrades to "the vendored JSON copy matches upstream", with nothing asserting the executor uses that value. The local self-skip (*"cargo test already proves the vendored copy parses"*, `toolchain_lock_parity.py:36-43`) becomes a genuine hole.

**S-04 — `check_doc_drift.py` goes red *because of* the relocation, and the tempting fix erodes it.** `check_doc_drift.py:239 harvest_tree(root / "scripts", "*.py")` (verified) treats `scripts/**/*.py` as an authoritative source of live identifiers — board names, `ALP_HW_BUILD_*`/`ALP_SOC_*` macros, board.yaml field identifiers. Move the generators and every doc token whose only definition lived there becomes "dead". It is `gate: true` **and** wired to `pr-doc-drift.yml`. The obvious fix is a hand-kept allowlist — precisely the erosion ADR-0020's Amendment documents for `normalize_plan`, and precisely what the gate's own docstring says it avoids.

**S-05 — `check_emit_registry.py` degrades to a `SystemExit` on the first file move.** It AST-parses the `--emit choices=[...]` out of `REPO/"scripts"/"alp_project.py"` and `REPO/"scripts"/"alp_orchestrate"/"cli.py"` (`:38-39`), hardcoded. It is the IDE/Studio emit contract's only drift gate.

**S-06 — The write-side CRLF gate cannot see tan; the read-side has no gate and a live tripwire.** `check_write_text_newline.py` scan roots are hardcoded `("scripts", "firmware")`, and it matches only `.write_text()` — `open(path, "w")` translates identically and is unchecked. `metadata/catalog.json` already carries UTF-8 at byte 5404; `check_e1m_pinout.py` already has four bare `read_text()` calls. Maintainer host: Windows 11. CI: ubuntu. The one Windows leg: `continue-on-error: true`.

**S-07 — There is no Python lint anywhere in alp-sdk.** Verified: zero hits for `ruff|flake8|black|mypy|pylint` across `*.yml/*.yaml/*.toml/*.cfg/*.sh/*.txt`. `.pre-commit-config.yaml` is clang-format-only and opt-in. tan-cli's blocking `cargo fmt --all --check` + `cargo clippy --all-targets --locked -- -D warnings` (`ci.yml:31-41`) has no successor. A straight downgrade the port must consciously accept or close.

**S-08 — Cross-platform proof drops to zero.** tan-cli tests on `[ubuntu, windows, macos]` (`ci.yml:43-49` — *"tan is a cross-platform CLI whose path handling (os.pathsep env append, .venv discovery, `.exe` suffixes) differs per host — test all three"*). alp-sdk cannot backstop it: `cross-platform-zephyr.yml:106-112` sets macOS and Windows `continue-on-error: true`, `check_cross_platform.py` is `gate: false` and soft-warn, and that workflow never invokes `tan` at all.

**S-09 — `tan-docs-drift.yml` is ADVISORY and its classifier is clap-shaped.** `check_tan_docs_surface.py:437-448` classifies a verb as forwarding by testing whether the `Usage:` line ends in the literal `[ARGS]...`. argparse/click print lowercase `usage:` and never emit that token — every forwarding verb silently reclassifies as native and the flag check then runs against help text that never contained clap-style listings. `gate: false`, daily cron, installs tan via `install.sh` from tan-cli's `main`. The port changes both the classifier's assumptions and the install path.

**S-10 — `getting-started.yml` in tan-cli is the only thing that runs the customer's literal path.** `getting-started.yml:18-25`: `shellcheck install.sh` (*"never linted by any workflow in either repo"*) → `./install.sh` (real release download + sha256 verify) → `tan bootstrap` → `west sdk install` → `tan init` → `tan build`. Deliberately **not** `paths:`-filtered (`:26-28` — *"A gate that skips itself on a \"docs-only\" PR reports green for a run that checked nothing"*). tan-cli's `first-blink` job (`parity.yml:542-627`) covers similar ground and deliberately does NOT pre-install west (*"`tan bootstrap` creating the venv and installing west into it is one of the things under test"*), asserting `../.west/config`, `../.venv`, `../zephyr`, `../modules` exist. **The two workflows' headers contradict each other about which was first — resolve which is the survivor before deleting either.**

**S-11 — seam-2 is the only executability proof, and it is Rust-built.** `tan-cli/.github/workflows/parity.yml:391` `cargo build --locked --bin tan`, deliberately routing through `tan build` not `west` (*"routing around the executor would test Zephyr and assert nothing about the seam"*). It asserts non-empty `data.written` (`:426-431` — *"an empty write list is exactly the silent-pass shape this seam exists to catch, so it is a failure here, not a pass"*) and an **ARM** ELF (`:474-478` — *"A host x86-64 binary here is the #97 defect reaching the artefact"*), selecting the m55_hp slice explicitly. seam-1 structurally cannot see this (`dispatch-tan-parity.yml:11-22`).

**S-12 — `pr-alp-build.yml`'s heterogeneous legs are dead, and its SoM axis is a no-op.** Verified: `example_path="examples/rpmsg-${som_short}"` (`:139-140`) and `examples/rpmsg-v2n/board.yaml` does not exist (the examples live at `examples/multicore/`); the skip logic at `:155-163` auto-skips with a `::notice::`. Its other two cells hardcode paths independent of `matrix.som` (`:142-147`), so the 4×3 matrix exercises exactly two `board.yaml` files. **Correction to one lens:** the `zephyr-only-slice` cell picks `gpio-button-led`, which fans out to three slices, so the byte-stability rebuild+diff (`:220-246`, no `continue-on-error`) *does* run against a 3-slice manifest — just never one with a real `ipc:` block.

**S-13 — Three Renode workflows wait on a capability that was retired.** `pr-renode-dual-os.yml:13-15` — *"`tan renode` ... is still a stub there, so the actual dual-OS boot + handshake assertion below is PENDING (tan-cli#77) -- kept as a no-op notice."* It has no `pull_request` trigger (`:41-46` is `workflow_run` + `workflow_dispatch`) and both artefact fetches are `continue-on-error: true` (`:113`, `:130`). With Renode retired, #77 never closes and it can never promote.

**S-14 — The ADR-0021 `${TOOLCHAIN_ROOT}` injection is schema-blocked today.** Verified: `slices[].env` is `additionalProperties: false`, `required: ['ALP_SDK_ROOT']`, `properties` = `{ALP_SDK_ROOT}` only. alp-sdk **cannot** emit `ZEPHYR_SDK_INSTALL_DIR`, while tan already resolves the token (`toolchain.rs:66`, `plan_tokens.rs`, `token_substitution.rs:244-254`). Collapsing the seam removes the obstacle and is the cheapest moment to close it.

**S-15 — Nothing runs the diagnostic-v1 conformance check.** No `test_check_diagnostic_schema.py`, no workflow reference. `check_diagnostic_schema.py:5-13` describes the strongest posture in the repo (*"A REAL document, produced by actually running `alp validate --format json` against a known-bad board.yaml fixture (never a hand-written sample)"*) — and I cannot determine that it executes anywhere.

---

## 3. BINDING ADRs

### 3.1 Must supersede

| ADR | Status quoted | Why |
|---|---|---|
| **0020** — SDK plans; standalone `tan` is the whole surface | *"Accepted — ... the cross-repo `repository_dispatch` trigger remains outstanding as a maintainer action, and the release train ran anyway"* (`:3-7`) | The port **inverts** its central split — the planner moves *into* the executor. Its Decision names Rust as a benefit (`:335-336`). Its "No `alp`-named command survives anywhere" (`:293-295`) is contradicted by its own Amendment 2, by `docs/adr/README.md:59` (*"the `alp` script and read-only `west alp-{emit,migrate,lock,quality}` remain"*), and by `scripts/alp_cli/` (20 modules, a `@click.group`). Amendment 7 says the `repository_dispatch` trigger *does not exist*; `dispatch-tan-parity.yml` exists and is machine-lockstepped. Its pinned-hash claim (`:316-317`, `:451`) is **false** — verified in `alp-sdk-vscode/src/alpCli/download.ts:11-16`: *"there's no checksum/signature check here."* Three of its four contract-completeness bullets (`:353-359`) were never implemented. |
| **0010** — Heterogeneous OS orchestration | *"Accepted — superseded by ADR-0020"* (`:3-4`) | Its CLI row is superseded a **second** time. Its schema/defaults/IPC half (`:51`, `:53`, `:176-179`) is live and untouchable. Its *"CI verifies the integration ... gates every PR rather than being a doc claim"* (`:76-79`) is false three ways (S-13). |
| **0014** — Build-plan emit CLI contract | *"Accepted — partially superseded by 0020"* (`:3`) | Its *properties* survive verbatim and are what tan reads. Its "cannot drift by construction" clause (`:54-56`) names a step Phase 4 deleted. Its unamended `:82-83` — *"Consumers pin to **release tags**, never `dev`"* — inverts once the planner ships inside tan; no ADR records that. |
| **0021** — Toolchain provisioning | ***"Proposed"*** (`:3-5`) | Half-shipped, and a Proposed ADR is not a decision. Lockfile half enforced; injection half blocked at the schema (S-14). Re-ratify or close, do not treat as binding. |

### 3.2 Must obey

- **ADR-0017** (`:34-35`) — the doctrine the port's two review gates derive from. See I-26.
- **ADR-0011** — intra-family portability only; `docs/portability-matrix.md:386-391` is now *programmatically regenerated* by `gen_portability_matrix.py` under `pr-generated-files`. That generator is planner-adjacent and moves with the planner; its drift gate stays in alp-sdk. **Re-point in the same change or a customer-facing guarantee silently stops being regenerated.** Same coupling for `gen_soc_caps.py` (ADR-0002 `:97-98`, *"Schema-then-generator is the rule"*).
- **ADR-0018** — `libraries:` is one declaration; the licence enum in `library-v1.schema.json` is exactly 8 permissive SPDX ids (*"a GPL-family or proprietary licence is rejected so a copyleft surprise cannot ride in through a `libraries:` selection"*). A legal-review artefact — the relocation must not loosen it.
- **ADR-0012** — Zephyr-on-M first-class on all three hosts; Yocto Linux-only by upstream constraint. Weakly enforced (S-08) and directly implicated in the PyInstaller problem.
- **ADR-0001** (`:24-29`) — *"An app written against `<alp/i2c.h>` recompiles unchanged across [Zephyr, Yocto, bare-metal]. This is the *central* justification."* Those three are exactly the `slices[].backend` enum. An MVP that serves only the Zephyr lane is a stated non-goal, not licence to design the executor MCU-shaped.
- **ADR-0015** — precedent *against* duplication: *"no mirrored header, no CI diff-check, no drift"*. Cite it when deleting the vendored comparator twin and the six oracle fixtures.
- **ADR-0019** — the cleanest in-repo example of the ADR-0017 pattern (one upstream source, one vendored projection, an offline gate). Cite it as the shape a relocated generator must keep. Freshness is deliberately un-gated.
- **ADR-0013** — `tfm-sysbuild-conf` is a planner emit that moves. Its M55-HP placement is *"implicit and documented at the schema, board-config doc, and security-audit-plan layers"* — it lives in no schema field, so nothing fails if the port drops it.
- **ADR-0005** — the dual-use acid test does not cover a **third** repo. The superseding ADR must state where a new artefact lands across alp-sdk / tan / alp-sdk-vscode.

### 3.3 Not yet an ADR — write **ADR-0022 early**

The plugin architecture, the ONE-extension rule, the never-learn-a-hardware-fact rule and the one-`board.yaml` invariant live **only** in `docs/superpowers/specs/`, which `docs/adr/README.md:10-12` does not govern (*"ADRs are append-only — a decision is never edited, only **superseded**"*). The master plan schedules "Supersede ADR-0020" as the last bullet of sub-project 4 — by which point four sub-projects of decisions will have been made against a record that still says the executor is Rust and lives above the SDK. **The three rules that are review gates with no mechanical enforcement are exactly the ones that need a number so a reviewer can cite them.**

ADR-0022 must also decide the fate of `scripts/alp_cli/` (`doctor`, `run`, `monitor`, `new-som`, `model`, `faultdecode`, `explain`, `validate`, `emit`, `generate`, `init`) and `alp-mcp`. Note: `pyproject.toml:47-52` deliberately ships **no** `alp` console script — *"`tan` ... is the sole user-facing command surface (ADR-0020 end-state B) ... invoked as `python -m alp_cli <sub>` — never as a user-installed `alp` binary."* The package is a backend, not a competing CLI — but the ADR README says otherwise and nobody has decided the verbs' fate.

---

## 4. FALSE PROMISES

Customer-facing statements that are not true today. Each is a port-blocking decision: fix the promise, fix the code, or record it.

1. **Exit codes.** `docs/cli.md:492` *"Hard schema/xref/consistency errors return 1."* and `AGENTS.md:82-83` say 1; `docs/getting-started.md:590-591` says 3. The golden-pinned contract says **validation = 2**, write = 3 (`contract/envelopes/validate-offline-schema-violation/expected.exit`). Three docs, one test-gated truth.
2. **`tan validate <path>` does not parse.** Verified: `error: unexpected argument 'board.yaml' found`. `ValidateArgs` has one field, `--offline` (`cli.rs:629-633`). Published in **gated** docs: `README.md:67`, `AGENTS.md:78`, `AGENTS.md:84`, `docs/cli.md:483-485`. `check_tan_docs_surface.py` is green because it checks flag *spelling*, never positionals.
3. **`--format sarif` does not exist.** Verified: `[possible values: text, json]`; `grep -rn sarif crates/` = zero. Documented at `docs/cli.md:485, 495, 503-506, 508`, including the SARIF one-based-region note. SARIF lives only on the Python side and tan never forwards to it. The default format is `text`, not `human`.
4. **`tan validate --format json` does not emit diagnostic-v1.** `docs/cli.md:497-502` promises `schemaVersion` + zero-based LSP ranges. The golden (`validate-offline-schema-violation/expected.json`) has no ranges, no positions, no ALP-B code — `tan-core/src/validate.rs:191-194`: *"The CLI only consumes `message` + `severity` (it rewrites the issue code to `validate.<outcome>`)."* An IDE author building an LSP provider from `docs/cli.md` targets a document tan has never emitted.
5. **`tan explain ALP-B001` does not exist — and implementing it breaks a golden-pinned surface.** Verified: `error: unexpected argument 'ALP-B001' found`. `ExplainArgs` is `--template` (`cli.rs:693-697`); `tan explain --help` = *"Explain a project/module template or a generation target"*. Documented as its own section at `docs/cli.md:623-628`. **`tan explain`'s real payload is `data.available.projectTemplates`, flagged in `contract/README.md:29` as the wizard's starter list.** A faithful implementation of the docs replaces the New Project wizard's data source with a diagnostic decoder — silently, because every extension read is behind `?? []`.
6. **Ten-plus example READMEs publish invocations that cannot parse.** `tan build <path>`: `examples/README.md:14, :285`; `examples/multicore/rpmsg-v2n/README.md:112`; `rpmsg-aen/README.md:67, :78`; `heterogeneous-offload/README.md:60, :71`; `rpmsg-imx93/README.md:75`; `v2n-gd32-bridge-functional/README.md:75`; `i2c-device-hub/README.md:63`; `vendor-ext-composability/README.md:36`. `tan build --board <string>` (no such flag anywhere): `power-managed-sensor/README.md:141`; `v2n-m1-deepx-inference/README.md:66`; `mproc-mailbox/README.md:47, :62`; `production-deployment/README.md:151`. `--core` on build: `rpmsg-v2n/README.md:134`. Several name `ensemble_e8_dk`, which `getting-started.md:534` says targets *"Alif's own EVKs, not the E1M board"* — wrong twice. `examples/**` is not in `check_tan_docs_surface.py`'s `DOC_SOURCES` (`:20-24`). **Do not add `--board` to make them true — that inverts I-01 and I-02.**
7. **"If you're targeting an M-class core only, you never need to leave macOS or Windows."** `docs/cross-platform-setup.md:887-889`. Verified false in the *default flash path*: `zephyr/boards/alp/e1m_aen{401,601,801}*/board.cmake` all set `board_set_flasher_ifnset(alif_flash)`, and Alif SETOOLS is a Linux-only bundle (`scripts/bench/aen/bench-env.sh:7-9` — *"the Alif SETOOLS are Linux-only. There is no native PowerShell equivalent"*). **Correction:** `alif_flash.py:283` is an f-string inside a `RuntimeError`; the SETOOLS dir is entirely user-supplied (`:200-208, :236-237, :261`). The constraint is a **vendor distribution** fact, not a code hardcode — "fix the hardcode" sends the port at the wrong file.
8. **ADR-0020's pinned-hash / signature verification does not exist.** `download.ts:11-16` — *"That proves the transfer completed, not that the bytes are the right binary — there's no checksum/signature check here."* Two consequences: PyInstaller packaging is **not** a security regression, and the superseding ADR must state the shipped state rather than inherit the sentence.
9. **`docs/cli.md`'s generate catalog is stale.** It says six targets and files tan-cli#113/#114/#115/#116 as open gaps (`:126-131, :381-387`). Real: `tan-core/src/loader.rs:200-210 ALL_EMIT_MODES: [&str; 9]` plus `zephyr-board`, deliberately reachable only via explicit `--target zephyr-board --core <id>` (`generate.rs:37-42`). All four "gaps" are closed.
10. **`docs/cli.md`'s forwarded-verb list is stale.** It names nine forwarders (`:62-66`); only four remain (`model`, `monitor`, `new-som`, `faultdecode`), per `cli.rs:443-446` and tan-cli README:271. **Caveat:** "not an argv-forwarder" ≠ "native" — `tan validate` spawns `validate_board_yaml.py` by default (`validate.rs:4-7, :134`), `tan generate` spawns `alp_project.py`, `tan build` spawns the SDK for the live plan. The real Python-spawning seam is much larger than the forwarder count implies.
11. **`llms.txt:63` maps `tan doctor` to `scripts/alp_cli/doctor.py`.** `docs/cli.md:57-61` correctly says it is native Rust with an unrelated check list. `llms.txt` is in no gate.
12. **tan-cli's README claims alp-sdk *"Ships an `alp` console script"*** (`:299-301`). `pyproject.toml` deliberately ships none.
13. **Both package-manager install paths resolve to nothing.** Honestly self-flagged (tan-cli README:171-179) but still published. `cargo install --path crates/tan-cli --locked` also appears in `docs/cli.md:40`, `docs/firmware-quickstart.md:69`, `docs/getting-started.md:37`, `README.md:65` — meaningless after a Python port, and `check_tan_docs_surface.py` does not check install commands.
14. **Two bootstrap front doors contradict each other.** `README.md:44-46, :65` tells customers `bash scripts/bootstrap.sh`; tan-cli README:231-232 says `tan bootstrap` *"runs natively on Linux, macOS and Windows and needs no `bash`"*, and `tan build` auto-bootstraps unless `--no-auto-bootstrap` (`cli.rs:511-516`). The alp-sdk Quickstart is also POSIX-only (`source ../.venv/bin/activate`).
15. **The envelope is documented with two field counts.** Six in `contract/README.md:7-8`, seven (with optional `sdk`) in tan-cli README:285-289. Read the goldens.
16. **`docs/heterogeneous-builds.md` is stale in three load-bearing places.** `:241-243` claims a `#error` for a blocked channel (the code emits a comment, I-16); `:236` claims *"Drift between the Linux DT and the Zephyr overlay becomes impossible"* (nothing gates address agreement); `:443` says `tan flash` walks `boot_order:`, which is always empty and, given `som-preset-v1.schema.json` is `additionalProperties: false` with **no** `boot_order` property (verified), is **unpopulatable without a SoM-preset schema change**. §9's `alp_rpc_open` pattern is also not what the bench-proven firmware does.
17. **The generated carve-out is not what the working V2N firmware uses, and the DTS says so.** `zephyr/boards/alp/e1m_v2n101_m33_sm/...cm33.dts:121-125` — *"Addresses are the Renesas RZ/V Multi-OS Package memory map, NOT the paper `ipc:` carve-out in this project's board.yaml (still ocram_low pending a follow-up that reconciles the two -- see #683)."* `examples/multicore/rpmsg-v2n/m33_sm/src/main.c:26-30` bypasses `<alp/rpc.h>` and no longer includes `<alp/system_ipc.h>`. **Do not "fix" the numbers to match the DTS — that breaks the frozen goldens. Treat #683 as open; the port must not silently resolve it either way.**
18. **The carve-out DT include is orphaned.** `zephyr/boards/alp_e1m_v2n101_m33_sm.overlay:22` includes `"../../build/generated/dts-reservations.dtsi"` — SDK-root-relative, while the plan writes under PROJECT_ROOT — and `:16-19` claims the orchestrator sets `EXTRA_DTC_FLAGS`. `grep EXTRA_DTC_FLAGS` across `scripts/` and `zephyr/` returns **only that comment**. Seven other overlays carry the same include and nothing in `scripts/` references any of them. Do not port a mechanism that is not wired; if it must be closed, `-I` belongs on the `west build` command line, not in a Kconfig fragment.
19. **`tan build` on native_sim: two docs disagree on whether it runs the binary.** `docs/getting-started.md:330-332` says it does; `docs/cli.md:117-119` and `README.md:65` say *"it never runs the produced binary itself (that's `tan run`)"*. `tan run` exists with its own `--flash` (`cli.rs:466-472`), which suggests getting-started is stale. Also `native_sim` is unreachable from `board.yaml` at all (`som.sku` is pattern-locked to `^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3})$`), so that branch is arguably dead.

**Resolved from a prior "cannot determine":** `alp-sdk-vscode/src/alpCli/service.ts:27` — `export const SUPPORTED_CLI_VERSION = "0.4.0";`. tan-cli's README:306-308 (claiming 0.2.0 and a binary named `alp`) is stale.

---

## 5. THE HETEROGENEOUS RULES

Stated so an implementer cannot get them wrong.

**R-1 — Enumerate slices from the SoC, not the customer.**
`soc_core_ids = [c["id"] for c in soc_spec["cores"] if "id" in c]`, then for each: merge `topology[core_id]` (base) with `board.yaml cores[core_id]` (override) **shallowly, per key**. Never deep-merge the nested `inference:`/`memory:`/`iot:`/`power:` dicts. Any `cores:` key not in `topology` is a hard error. Any `topology` key not in SoC `cores[]` is a hard error. Any SoC core with neither is a hard error.

**R-2 — Resolve each core's OS from its class, then validate.**
`cortex-a*` → yocto, `cortex-m*` → zephyr, else `off`. Accept only `off`/`baremetal` as explicit overrides. Reject the cross-class value with the exact message. Then per-OS: zephyr/baremetal need `app:`; yocto needs `app:` or `image:`; `off` is exempt. **Run the per-OS check before the class check** — error precedence is observable.

**R-3 — Two emitters, two enumerations. Memorise both.**
- Build plan: `sorted(coreId)`, `off` **excluded**.
- System manifest: SoC `cores[]` **array order**, `off` **included**.

**R-4 — Resolve carve-outs in exactly this order.**
1. If any entry has `kind: rpmsg`, require a resolved `mailbox.controller` (not `None`, not `"TBD"`) **and** a channel with `reserved_for: alp_default_rpmsg`. Otherwise every rpmsg entry blocks.
2. Sort entries **alphabetically by name**.
3. Per entry: candidate regions = those whose `accessible_from` ⊇ the entry's endpoint set. Rank by `(0 if cacheable == preference else 1, region_size_bytes or 1<<62)`; preference defaults to **non-cacheable**. Take `candidates[0]`. No candidate → blocked with a reason.
4. Region top = `align_down(base + size, 4096)`, initialised once per region.
5. Size = `ceil(carve_out_kb * 1024 / 4096) * 4096`.
6. Allocate **top-down**: `new_top = region_top - size`; block on underflow; then `region_top = new_top`.
7. Explicit `address:` → check 4 KiB alignment **only**; do **not** range-check, do **not** overlap-check, do **not** move the high-water mark. (Reproduce the hole; do not close it silently.)
8. `h = fnv1a_32(name.encode("utf-8"))`; `low = h & 0xFF`; if `low` already seen, **block this entry** — do not renumber, do not raise. Else `src = 0x400 | low`, `dst = src + 1`.
9. Mailbox channel: the channel whose `reserved_for == entry.name`, else `0`.
10. Resolved entries report the **chosen region's** cacheable flag; blocked entries report `False` and `region: ""`.

**R-5 — Resolve storage partitions in exactly this order.**
Devices `sorted()`; entries within a device sorted by name; sizes page-rounded up; allocate **bottom-up** from offset 0; explicit `offset_kib` honoured (page-aligned) and does **not** move the high-water mark; overlap **is** checked here. Resolved reports the **aligned** size; blocked reports the **declared** size. `base_kib` is a device offset, never a physical address. `capacity_mbit` → bytes is `* 1024 * 1024 // 8`.

**R-6 — Never abort on incomplete metadata.**
Every unresolvable case is `status: blocked` + a human `reason:`. The emit exits 0. Blocked IPC still emits all six macros as zero stubs with a **comment**, never a `#warning` (`-Werror=cpp`). Blocked partitions emit only a comment — no `reg`, no `size_kib`.

**R-7 — Every IPC endpoint must be a core in the project with `os != off`.** Validate this *before* carve-out resolution, so the resolver can assume live cores.

**R-8 — Materialisation ordering.**
Materialise **all** `sharedArtefacts` (`generated/system_ipc.h`, `generated/dts-reservations.dtsi`, `generated/dts-partitions.dtsi`, `build/alp_sysbuild.conf` when present) **and** write `system-manifest.yaml` at `<buildRoot>/system-manifest.yaml` — with `generated/` as its sibling — **before dispatching any slice**. Slices may then run in parallel. There is **no slice-to-slice ordering**: `boot_order` is always `[]` and cannot be populated without a SoM-preset schema change. Do not build flash/boot sequencing on it.

**R-9 — Per-slice command rules.**
Zephyr: `west build -b <full qualified board>` with `cwd = buildDir` and **no `-d`**; existence is checked on the **bare** board name against `zephyr/boards/alp/*/board.yml`; missing tree → `command: null` + `board-tree-missing`. `-DEXTRA_CONF_FILE=<tokened alp.conf>` **only when not `--sysbuild`**. `-DSB_CONF_FILE` is a `;`-joined absolute forward-slashed list, family base first. No `--runner`. Yocto: `image:` → `bitbake <image>`; `app == "alp-image-edge"` → `bitbake alp-image-edge`; `app:` without `recipe:` → `command: null` + `yocto-recipe-missing`; else `bitbake <recipe>`.

**R-10 — Tokenise every checkout/project-anchored path; leave `boardYaml` alone.**
`PROJECT_ROOT` before `SDK_ROOT`. A path under neither raises from the single tokenizer and is caught into `command-unrooted` / `appdir-unrooted`. `buildDir`, `command.cwd` and `configArtefacts[].path` stay **relative** to the build root; the same paths reappear absolute-tokened inside command args.

**R-11 — Things a heterogeneous build needs that the plan does not carry.**
Helper-MCU firmware (manifest only), `ALP_SYSTEM_MANIFEST_PATH` for bitbake, `ALP_GENERATED_DIR` for a standalone Linux-side build, and the IPC Kconfig itself — `grep -c "IPC_SERVICE|OPENAMP|CONFIG_MBOX|rpmsg" scripts/alp_orchestrate/kconfig.py` returns **0**. Declaring `ipc:` generates addresses, a DT reservation and a manifest entry; the app's own `prj.conf` must turn the transport on. A port that assumes "`ipc:` declared ⇒ link wired" ships a project that compiles clean with no RPMsg at runtime, and no golden can catch it.

---

## 6. WHAT A PYTHON PORT WOULD PLAUSIBLY GET WRONG

Ranked by (likelihood × silence × blast radius).

**1. Relocate the planner out of `scripts/alp_orchestrate/` and turn off six gates while everything stays green.** (S-01.) The port's whole purpose is this move. Nothing detects a stale path filter. *Mitigation:* update every `paths:` list, `check_emit_snapshots.py:41`'s invocation target, and `check_emit_registry.py:38-39`'s AST paths **in the same commit**, and add a test that asserts each filter still matches the planner's real location.

**2. Sort the system-manifest by `coreId`.** (I-06.) It is the obvious, tidy, symmetric thing to do; the plan already does it; and it fails `check_emit_snapshots` on every AEN project. Two different rules, one per emitter.

**3. Join `EXTRA_ZEPHYR_MODULES` with `os.pathsep`, and append without de-dup.** (I-10.) `os.pathsep` is what a Python developer reaches for. It is `;` on **every** platform. Correct-by-accident on Windows, broken on Linux; and blind append double-registers the SDK as a Zephyr module on a second invocation. Copy `plan_exec.rs:20-42`, not `execute/env.rs`.

**4. `Path.write_text()` without `newline=""` and `read_text()` without `encoding="utf-8"`.** (I-27, S-06.) Windows maintainer, ubuntu CI, `continue-on-error` Windows leg, and a UTF-8 tripwire already sitting in `metadata/catalog.json` at byte 5404 with four existing bare-read violations in `check_e1m_pinout.py`. The write gate cannot see tan by construction; the read side has no gate at all.

**5. Prompt on a non-TTY and hang forever.** (I-23.) Three separate scars in tan's own history. Python's `input()`/`click.prompt`/`inquirer` inherit none of the guard. `--format json` must imply non-interactive because the extension drives everything that way. Failure is a CI job that hangs to timeout with no output.

**6. Print `tan.py 0.4.1` from argparse's `--version`.** (I-22.) The consumer regex is `/^tan \d+\.\d+\.\d+/` against the **first stdout line**. `prog` defaults to the script name. Any import-time warning on stdout also breaks it. Every version-gated extension feature turns off, silently, on both sides.

**7. Drop the `sdkCommit` split-brain guard because the schema says "informational".** (I-28.) The docs and the code disagree and the docs are the ones a porter reads. The result is builds against a different SDK checkout than the plan was captured from.

**8. Re-implement token substitution as `str.replace()`.** (I-29.) Four distinct hard refusals vanish, including the one that catches an unresolved `${SDK_ROOT}` degrading into the bare path `/scripts` — which sails past the leftover-token guard because there is no token left to catch.

**9. Delete `contract.rs` before a Python golden harness exists.** (I-21.) 15 fixtures, the exit ladder, five frozen codes, `--version`, `--build` acceptance, and the `doctor --build` key set. Every one fails open in the extension. The fixtures are language-neutral data (`args.txt`, `expected.exit`, `expected.json`) — re-drive them from Python, spawning a real subprocess.

**10. Rename or relocate `scripts/alp_project.py`.** (I-31.) It is tan's hardcoded SDK-root marker in six places, and the natural casualty of "the planner is Python now, why keep two entry points". Breaks `--sdk-root`, `doctor`, `clean`, `generate`, and blinds `check_emit_registry.py`.

**11. Fix the emit-snapshot goldens' `sdkVersion` churn by normalising it.** (I-17.) The two gates disagree on purpose; the goldens keep it real; `bump_version.py` regenerates `alp.lock` and the ABI snapshot but **not** the snapshots. Every release bump hand-breaks six goldens, and the port ships during a version-bumping period.

**12. Tighten `warnings[].code` into a closed `Enum`.** (I-11.) Idiomatic Python, explicitly forbidden by the schema, and forward-incompatible.

**13. Make `executionPolicy` / `planPathMode` required at the root because the emitter always emits them.** (I-11.) Exactly the regression ADR-0020 Amendment 3 documents, reverted rather than bumped because the consumer pins `schemaVersion == 1`.

**14. Tokenise `boardYaml` for consistency.** (I-09.) Destroys tan's PROJECT_ROOT resolution.

**15. Emit tan's own version into `sdkVersion`.** All six build-plan goldens go red at once and it reads as a planner regression.

**16. Validate the schema after `_normalize_libraries`.** (I-37.) A natural refactor; every core-scoped-library project then fails with an `additionalProperties` error.

**17. "Fix" `min_ram_kib` vs `soc_ram_kb` unit mismatch.** (I-41.) Newly rejects working projects.

**18. Normalise the `heap_kib` / `stack_kib` zero-semantics asymmetry.** (I-46.) Looks like a bug, is not, and is untested.

**19. Add an overlap check to explicit carve-out `address:`.** (I-14.) Newly *blocks* projects that emit today; a behaviour change needing a golden update and a CHANGELOG note, not a drive-by fix.

**20. Unify the two allocators (carve-out top-down, partition bottom-up) or the two validators (loader schema-only vs the CLI's three-pass).** (I-14, minor list.) Both are deliberate; unifying either changes emitted bytes or stderr for every plan.

**21. Reorder the Kconfig sections for readability.** (I-45.) Changes which `CONFIG_HEAP_MEM_POOL_SIZE` wins on an inference project with an explicit `heap_kib`.

**22. Ship `tan init` shelling the SDK instead of reading the vendored scaffold tree.** (I-32.) Silently gives `tan init` an alp-sdk dependency it deliberately does not have, while the byte-parity script keeps passing.

**23. Collapse the `ALP_FLASH_FORCE` `"planned"` status back into `"ok"` — or drop the gate.** (I-30.) Hardware writes. Undocumented in alp-sdk. Belongs at the top of any pre-merge checklist for the flash path.

**24. Convert the two Ethos-U `ValueError`s to `OrchestratorError` "for consistency".** (I-48.) The traceback is today's observable behaviour and no test covers either raise.

**25. Reject an unknown `slices[].backend` at parse time.** (I-12.) Closed-enum parsing is the Pythonic default and it fails the whole plan document before `executionPolicy.unknownBackend` can be consulted per-slice — the exact regression tan already fixed.

**26. Treat `--sdk-root` as "validate then fall through".** (I-31.) `if not isdir(p): continue` turns a typo into a silent build against a different SDK.

**27. Assume `len(endpoints) == 2`.** (I-54.) Schema permits N; the resolver is generic.

**28. Hardcode the `os:` enum.** (I-36.) Compiles, passes every gate, and reintroduces exactly the drift the schema-read exists to prevent. Note the repo already half-does this at `topology.py:63`.

**29. Fix `check_doc_drift` with an allowlist when it reds from the relocation.** (S-04.) The cheap response that erodes the gate — the same shape ADR-0020's Amendment records for `normalize_plan`.

**30. Serialize the envelope with Python defaults.** (I-21.) `null`-vs-absent asymmetry, `schemaVersion` as int rather than `"1"`, `ok` computed independently of `exitCode`, `null` where the goldens have `""`.

---

### Standing review checklist (paste into the port's PR template)

- [ ] Does this change any of the 35 emit goldens? If yes, is the delta intentional and recorded?
- [ ] Does this move a file named in any workflow `paths:` list, `check_emit_snapshots.py:41`, or `check_emit_registry.py:38-39`? Updated in the same commit?
- [ ] Does this touch `metadata/**` or `metadata/schemas/`? Regenerate `alp.lock` **from an LF-native clone**.
- [ ] New/removed `scripts/check_*.py`? Registry updated in the same commit (`check_quality_registry.py` will catch you).
- [ ] Any `write_text()` → `newline=""`. Any `read_text()`/`open()` → `encoding="utf-8"`.
- [ ] Does tan now carry a SKU, address, pin name, I²C address, or vendor branch? **Reject** (ADR-0017; no gate will catch it).
- [ ] Does this add a second command surface, a per-SKU extension, or an OS/backend selector? **Reject.**
- [ ] Any cargo test being deleted? Name its Python successor in the same PR.
- [ ] Frozen-surface touched (envelope, exit codes, 5 frozen issue codes, `--version` first line, `doctor --build` keys, 8 asset names)? Golden updated *and* consumer coordinated?