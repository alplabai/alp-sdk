# Model & Edge-AI Management in VS Code — cross-repo design

- **Date:** 2026-07-24
- **Status:** Design (brainstormed + approved section-by-section; pending written-spec review)
- **Repos touched:** `alp-sdk` (pipeline + device runner), `tan-cli` (executor), `alp-sdk-vscode` (GUI)
- **Spec home:** `alp-sdk` (owns the model pipeline the contract is derived from)
- **Supersedes/extends:** the unified AI-accelerator model pipeline (`docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md` and the Stage-1a…Stage-2 plans). This spec adds the *management surface* on top of the finished pipeline; it does not change the pipeline itself.

## 1. Problem

`alp-sdk` already handles NPU / edge-AI in a unified way: a model declared in `board.yaml` `models:` compiles (compile-what's-available) into a fat multi-backend `.alpmodel` package, and an on-device loader (`alp_inference_open_alpmodel()`) selects the blob matching the device's NPU. That whole pipeline is host/CLI + on-device C — it is **not reachable from the VS Code extension** today.

The extension is a thin GUI over the build CLI (`tan`). It shells `tan` and parses a fixed JSON envelope. Its only model touchpoint is hand-editing `board.yaml` `models[]` in the Configurator "AI models" card — there is no compile action, no artifact inspection, no toolchain awareness, no deploy/run. The model-pipeline CLI (`alp model build`, Python, in `alp-sdk`) is not exposed through `tan`, so the extension's CLI seam cannot reach it at all.

Goal: make VS Code the end-to-end surface for model + edge-AI management — declare → compile → package → inspect → deploy → run/observe — without breaking the thin-extension / single-executor architecture.

## 2. Locked decisions

1. **Scope = full e2e:** declare → compile → package → inspect → deploy → run/observe. The device run/observe stage is explicitly phased behind unshipped runtimes (§8).
2. **Target user = both**, one design: the run/observe transport is an abstraction with two providers — `local-serial` (SDK customer, USB-attached board) and `bench-labgrid` (internal Alp Lab engineer). The NPU-toolchain doctor spans the full range from CPU-only up to the full stack.
3. **Compile location = local host now**, cloud remote builder as a designed-for future seam (reserve the boundary; do not build the backend — see §9 Out of scope).
4. **Pipeline spine = "tan wraps Python; extension stays thin"** (approach A). The `alp-sdk` Python pipeline stays the source of truth and gains `--format json` envelope output; `tan` adds a `model` command group that shells it and re-emits the envelope; the extension shells `tan model *` and renders the envelope. Rejected: re-implementing the pipeline in Rust (duplicates finished work; NPU tools are Python-ecosystem), and shelling the Python CLI directly from the extension (breaks thin-extension + single-executor / ADR-0020).

## 3. E2E flow across the repos

```
DECLARE       board.yaml models[]               extension Configurator + alp-sdk schema
   |
COMPILE       alp model build                    tan model build -> alp (Python) -> vela / dxcom / DRP-AI TVM subprocess
   |          => fat .alpmodel (multi-backend blobs + CBOR manifest)   [local host]
   |
INSPECT       coverage / sizes / requires        tan model info -> envelope -> extension Models panel
   |
DEPLOY        flash firmware + model             extension -> tan build / flash
   |
RUN/OBSERVE   run inference, read result         runner harness fw -> serial result line -> tan model run -> extension
```

Everything the extension renders comes from a `tan model *` envelope. `tan` is a thin orchestrator over the finished Python pipeline. Compilation runs on the local host. The only new cross-repo surface is the contract in §4.

## 4. The `tan model *` envelope contract (shared foundation — freeze first)

Every command emits the standard envelope `{command, ok, exitCode, project, data, issues}` plus a `contractVersion` field guarded by `tan`'s existing SDK version-skew mechanism (mirror the build-plan consumer guard). On a major-version mismatch the extension surfaces an actionable "update `tan`/SDK" error rather than rendering stale data, consistent with how build-plan skew is handled.

| Command | `data` payload | Source in `alp-sdk` |
|---|---|---|
| `tan model doctor` | `toolchains: [{backend_id, tool, available, version, reason}]` — `backend_id` ∈ `ethos_u` / `deepx_dxm1` / `drpai` / `cpu`; `tool` ∈ `vela` / `dxcom` / `drpai-tvm`; `reason` ∈ e.g. "not on PATH" / "no license" / "needs WSL" | each adapter's `is_available()` + a version probe (`scripts/alp_model/adapters/{cpu,deepx,drpai,ethos_u}.py`) |
| `tan model build [--model NAME]` | `models: [{name, source, alpmodel_path, total_bytes, targets: [{backend_id, silicon_ref, status, reason, blob_format, blob_bytes, requires: {sram_kib, …}}]}]` — `status` ∈ `compiled` / `skipped` | `alp model build` (`scripts/alp_cli/model.py`, exists). compile-what's-available warnings go in `issues[]` (missing tool = warning, not error) |
| `tan model list` | `models: [{name, source, declared_compile, artifact: {exists, path, bytes, built_at, stale}}]` — `stale` = source mtime newer than the `.alpmodel` | new `list` subcommand |
| `tan model info NAME` | decoded manifest: every target blob (`backend_id`, `silicon_ref`, `blob_format`, `blob_bytes`, `requires`) + a SoM-declared-backends × has-blob **coverage matrix** + `ALP_ERR_NO_FIT` risk flags | new `info` subcommand + a host-side manifest decoder (reuse the canonical-JSON sidecar from `scripts/alp_model/manifest.py`; the on-device C reader `src/common/alp_model.c` already exists) |
| `tan model run NAME` | `{backend_ran, latency_ms, outputs \| top_k, peak_sram_kib, err}` | **phased/gated** — needs the device transport (§6) + the on-device result contract (§5.4) |

## 5. `alp-sdk` change-set (pipeline gains an envelope + a device runner)

1. **`--format json` envelope on the `alp model` CLI** — reuse the writer that `validate` / `generate` already use (`scripts/alp_cli/validate.py` is the reference implementation). File: `scripts/alp_cli/model.py`.
2. **New subcommands `list` / `info` / `doctor`** (`build` exists). `doctor` calls each adapter's `is_available()` + a version probe; `info` decodes a built `.alpmodel`. File: `scripts/alp_cli/model.py` + `scripts/alp_model/`.
3. **Host-side manifest decoder for `info`** — the on-device C reader exists (`src/common/alp_model.c`) and the host writer emits canonical JSON (`scripts/alp_model/manifest.py`). Reuse the canonical-JSON sidecar rather than hand-decoding CBOR on the host.
4. **Run/observe device runner + result contract** (phased). A runner app loads the `.alpmodel` via `alp_inference_open_alpmodel()`, runs one inference on a bundled input, and prints exactly one structured result line over the serial console: `{backend_ran, latency_ms, top_k, peak_sram_kib, err}`. This is the on-device telemetry that does not exist yet. The Zephyr / Ethos-U runner (M-class) ships first; the A55 / Yocto runners for DEEPX (`dx_rt` over PCIe) and DRP-AI (DRP-AI TVM runtime) are bench-gated (§8). Home: a new harness/example under `examples/ai/`, plus the Yocto side for A55.
5. **Cloud-build seam (design-only)** — reserve `alp model build --remote <url>` + a corresponding envelope field. Not implemented in v1.

## 6. `tan-cli` change-set (thin orchestrator + device transport)

1. **`tan model` command group** dispatching `build | list | info | doctor | run` → resolve the SDK root (existing resolver) → shell `alp model <sub> --format json` in the SDK context → re-emit as `tan`'s own envelope. Pure orchestration; no model domain logic enters `tan-core`. Home: `crates/tan-cli`.
2. **`contractVersion` guard** for the model envelope, mirroring the build-plan skew guard. Home: `crates/tan-core`.
3. **Device-transport abstraction** for `tan model run` — two providers: `local-serial` (open the USB serial port, flash the runner via the existing flash path, read the single result line) and `bench-labgrid` (internal), selected by config/flag. This is the newest and most uncertain part of the design; it is isolated behind a provider interface so the local-serial path can ship independently. Home: `crates/tan-cli`.

Note on ADR-0020: this is additive to `tan`'s consumer role. The model logic stays in `alp-sdk`; `tan` only orchestrates and re-envelopes. Do not pre-empt Hakan's `alp`→`tan` binary repoint; wire against whichever binary name the extension currently resolves.

## 7. `alp-sdk-vscode` change-set (all thin, all shell `tan model *`)

| # | Change | Home | Rides existing pattern |
|---|---|---|---|
| 1 | **Models panel** — lists `models[]` from `tan model list`; per-model source, artifact status (built/stale/missing), backend-coverage chips from `tan model info`. Add/remove/edit **delegates to the existing Configurator "AI models" card** — reuse, do not duplicate | `packages/alp-webview/src/features/models/` | Configurator card `ConfiguratorView.tsx:1414-1538` |
| 2 | **Message types** `ModelsDataMessage` / `ModelBuildProgressMessage` / `ModelDoctorDataMessage` — added to **both** `src/ideHub/messages.ts` and `packages/alp-webview/src/types.ts` in lockstep (manual mirror) | 2 files | state slices `messages.ts:32-245`; progress mirrors `sdkInstallProgress` |
| 3 | **Commands** `alp.buildModel` / `alp.buildAllModels` / `alp.inspectModel` / `alp.modelDoctor` / `alp.runModel` via the envelope path | `package.json` + `src/alpCli/` | `runEnvelopeCommandAsync` (`adapterCore.ts:148-199`), parse (`service.ts:191-238`) |
| 4 | **`.alpmodel` artifact inspector** — coverage matrix (SoM declared backends × has-blob), each blob's `requires` SRAM vs `ALP_SOC_NPU_ARENA_SRAM_KIB`, `ALP_ERR_NO_FIT` flags | Models panel detail view | `tan model info` data |
| 5 | **NPU toolchain doctor** driven by `tan model doctor`; guides the user to install missing tools. **Guide, do not auto-install; `dxcom` is license-gated → no login-gated download link in this public repo** | `src/toolchain.ts` pattern | Toolchain-Doctor pattern `toolchain.ts:257` |
| 6 | **Deploy + run/observe UI** — "Run inference" flashes the runner, then shows `{backend_ran, latency_ms, top_k, peak_sram_kib}` from `tan model run`. Gated behind the Yocto runtimes for A55 NPUs; Ethos-U / CPU first | new surface | existing west/`tan` flash |
| 7 | **LSP `models` field docs** — add `models` to `FIELD_DOCS` / `CHILD_KEYS` (currently absent, so hover/completion doesn't cover it) | `src/lsp/service.ts:96-122` | existing LSP field docs |

Progress reporting for a long compile mirrors the SDK install/progress/cache pattern (`src/ideHub/sdkManagerMessages.ts:151-247`). No new npm/cargo/pip dependency is introduced in any repo.

## 8. Phasing / build order

Cross-repo order within a phase is **alp-sdk → tan-cli → extension** (the consumer cannot be built before the envelope it parses exists). Each phase is independently shippable.

| Phase | Delivers | Repos | Gate / dependency |
|---|---|---|---|
| **0 — Contract + thin wins** | Freeze the `tan model *` envelope spec. Ship two zero-dependency extension wins: LSP `models` field docs + a **read-only** Models panel built from the existing `acceleratorAvailability(som)` data (`packages/alp-core/src/sdkCatalogue/derive.ts:34-47`) | extension | none — immediate value |
| **1 — Author-time e2e** | `alp model` `--format json` + `list`/`info`/`doctor` → `tan model build/list/info/doctor` → extension build action + artifact inspector + toolchain doctor. **declare → compile → package → inspect works end-to-end, local host.** Ethos-U + CPU everywhere; DEEPX / DRP-AI where local toolchains are present | all 3 | local host; `dxcom` needs WSL + license, degrades gracefully |
| **2 — Deploy** | "Flash firmware with model" wired through the existing west/`tan` flash surface | extension (+ small `alp-sdk`) | none beyond Phase 1 |
| **3 — Run/observe** | Runner harness + serial result contract + transport abstraction. **Ethos-U / Zephyr + CPU shippable first.** A55 DEEPX / DRP-AI run/observe **gated on the bench-gated Yocto `dx_rt` / DRP-AI TVM runtimes landing** (not shipped today) | all 3 + bench | ⚠ blocked on unshipped Yocto A55 NPU runtimes |
| **Future — Cloud builder** | `tan model build --remote` posts to a build service holding licenses + heavy toolchains server-side. **Design-only seam now**; likely Hakan / separate-infra (OTA-server ownership pattern) | seam reserved in all 3 | out of v1 scope |

Phases 0–2 are fully achievable with what exists today. Phase 3 partially ships (Ethos-U/CPU); its DEEPX/DRP-AI half is hard-blocked on runtimes that are still bench-gated and unwritten. The design reserves the seams so nothing is re-architected when they land.

## 9. Out of scope (v1)

- **Cloud remote build backend** — only the `--remote` seam + envelope field are reserved; the service itself is not built.
- **Model registry / versioning** — v1 manages `board.yaml`-declared models + local source files + built `.alpmodel` artifacts in the build dir. No shared/versioned model catalogue.
- **On-device inference beyond a single result line** — no streaming telemetry, live dashboards, or continuous inference; run/observe reads one structured result per invocation.
- **Silicon-determined fields becoming customer-facing** — backends stay derived from `som.sku` → SoM preset; the customer never specifies a backend (unchanged from the pipeline design).

## 10. Risks & dependencies

- **Contract drift** — the envelope is the single cross-repo coupling. A field rename on either side breaks the extension silently (no compile error). Mitigation: the `contractVersion` guard + the envelope contract owned in this spec; change producer and consumer in lockstep.
- **Python-runtime dependency on the `tan model` path** — `tan model *` shells the SDK's Python. The SDK clone is already Python-based and present on the user's machine (via the SDK install flow), so this is satisfied, but `tan model doctor` should report a missing/broken Python as a first-class `reason`.
- **`dxcom` reality** — license-gated, Linux-only wheel, >15 GB RAM. On a Windows customer box it degrades to "skipped" unless WSL + a license are present; the doctor must make this legible, and no login-gated download link may appear in the public repo.
- **Bench-gated A55 runtimes** — Phase 3 DEEPX/DRP-AI run/observe cannot complete until the Yocto `dx_rt` / DRP-AI TVM runtimes exist and are bench-validated on real silicon.
- **Serial transport newness** — the `local-serial` provider is the least-proven piece; keep it behind the provider interface so Phases 0–2 don't depend on it.

## 11. Success criteria

- **Phase 0:** the extension documents the `models` field on hover, and shows a read-only Models panel derived from SoM data, with the `tan model *` contract frozen in this spec.
- **Phase 1:** from VS Code, a user declares a model, runs "build model", and sees per-backend coverage + `.alpmodel` sizes + `requires`-fit for their SoM — driven entirely by the `tan model` envelope, local host.
- **Phase 2:** a built `.alpmodel` is flashed to a board through the existing flash surface.
- **Phase 3 (Ethos-U/CPU):** "Run inference" returns `{backend_ran, latency_ms, top_k, peak_sram_kib}` into VS Code from a real board over the `local-serial` transport.

## 12. References

- Unified pipeline design + stage plans: `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md`, `docs/superpowers/specs/2026-05-27-stage2-npu-compiler-integration-design.md`.
- Pipeline internals (verified 2026-07-24): `scripts/alp_cli/model.py`, `scripts/alp_cli/validate.py` (envelope reference), `scripts/alp_model/manifest.py`, `scripts/alp_model/adapters/{cpu,deepx,drpai,ethos_u}.py` (`is_available()`), `src/common/alp_model.c`, `src/backends/inference/alp_model_select.{c,h}`, `src/common/alp_model_loader.c`, `include/alp/inference.h` (`alp_inference_open_alpmodel()`), error codes `ALP_ERR_NO_FIT` / `ALP_SOC_NPU_ARENA_SRAM_KIB`.
- Extension touchpoints (from the gap map, `alp-sdk-vscode`): `packages/alp-webview/src/features/configurator/ConfiguratorView.tsx:1414-1538`, `packages/alp-core/src/sdkCatalogue/derive.ts:34-47`, `packages/alp-core/src/configurator/service.ts:20`, `packages/alp-core/src/board/models.ts:161-172`, `src/ideHub/messages.ts:32-245`, `src/alpCli/adapterCore.ts:148-199`, `src/alpCli/service.ts:191-238`, `src/ideHub/sdkManagerMessages.ts:151-247`, `src/lsp/service.ts:96-122`, `src/toolchain.ts:257`.
- `tan-cli`: `crates/tan-cli`, `crates/tan-core`, the build-plan consumer contract + version-skew guard.
