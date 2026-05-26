# Unified AI-accelerator model pipeline — design

- **Date:** 2026-05-26
- **Status:** Draft for review (brainstorming output → writing-plans next)
- **Scope:** A single "model" concept that flows **compile → package → load → run** portably across every AI-accelerator backend the SDK targets (Arm Ethos-U, Renesas DRP-AI3, DEEPX DX-M1, CPU/TFLM).
- **Builds on:** `<alp/inference.h>`, `<alp/backend.h>`, `src/inference_dispatch.c`, `src/backends/inference/*`, the SoM-preset metadata, and `scripts/alp_*.py` orchestration.

---

## 1. Problem & current state

The runtime *scaffold* for inference already exists and is more complete than the "we haven't done much" framing suggests — but the **compiler toolchains and the cross-backend model artifact are empty space**, and three of four backends are stubs.

What exists today (on `dev`/`main`):

- **Runtime API** — `<alp/inference.h>`: `alp_inference_open(cfg)` → handle, `num_inputs/outputs`, `get_input/get_output`, `invoke`, `close`, `capabilities`. Shape is `[ABI-STABLE]`; per-backend bodies land incrementally.
- **Backend registry** — `<alp/backend.h>`: `ALP_BACKEND_REGISTER(class, name, {...})` into a per-class linker section; `alp_backend_select("inference", ALP_SOC_REF_STR)` picks by **priority → exact-silicon_ref-vs-wildcard → vendor strcmp**. Dispatcher: `src/inference_dispatch.c`; per-backend ops vtable: `src/backends/inference/inference_ops.h`.
- **Working backends** — `tflm.cpp` (CPU), `ethos_u_aen.cpp`, `ethos_u_n93.cpp`, `sw_fallback.c`.
- **Stub backends** — `drpai_v2n_stub.c`, `deepx_dxm1_stub.c` (every op returns `ALP_ERR_NOT_IMPLEMENTED`; issues #58/#59).
- **Metadata** — each SoM preset (`metadata/e1m_modules/<SKU>.yaml`) declares `inference.preferred_backend` (e.g. V2N101=`drpai`, V2M101=`deepx_dxm1`, AEN=`ethos_u`) and `inference.npu_population[]`. Backend selection is **silicon-determined** (the customer-facing `inference.backend` was removed from `board.yaml`).
- **Escape hatches** — `<alp/ext/renesas/inference.h>`, `<alp/ext/deepx/inference.h>` for vendor-specific knobs.

The gaps this design fills:

1. **No compiler unification.** Vela (Ethos-U), Renesas DRP-AI TVM (`vendors/renesas-rzv2n/rzv_drp-ai_tvm/`), and DEEPX `dx-compiler` are three separate host toolchains with no shared entry point or output artifact. There is no SDK-level "compile this model for my target".
2. **No cross-backend model artifact.** A model is a raw blob + a format enum; nothing ties one logical model to its several backend builds, records coverage, or carries the metadata a loader needs (tensor I/O, arena size, silicon validity).
3. **Model-handling gaps.** `preferred_backend` is declared but **unused by code** (purely informational today); arena sizing is "size it empirically"; no compile-time/load-time check that the shipped model matches the target backend; capability discovery is unpopulated on the stub backends.

---

## 2. Goals / Non-goals

**Goals**
- One declarative way to attach a model to a project; one artifact that runs on any matching board; one runtime call to load+run it regardless of NPU.
- "Simple yet powerful": *add a model = name + source*; everything silicon-specific is **derived**, not hand-specified.
- Graceful behaviour when a proprietary compiler is absent (partial package + clear runtime story).
- Build strictly on the existing registry + `<alp/inference.h>`; the loader is a thin selection layer, not a rewrite.
- **Capability-aware:** a model is matched to an NPU's *actual* envelope (MACs / SRAM / op-set), not just its backend type. A model too big for a small core (e.g. won't fit an Ethos-U55-128) is caught at compile time (recorded as a capability miss) and at load time (clear error / CPU fallback) — never silently mis-run on a part that can't host it.

**Non-goals (v1 — YAGNI)**
- Generic post-training quantization / calibration. You bring a model each backend's compiler accepts; vendor compilers do their own quantization.
- PyTorch as a first-class input. PyTorch → ONNX export stays a **documented pre-step**, not pipeline-owned.
- ExecuTorch backend (`ALP_INFERENCE_MODEL_EXECUTORCH` exists in the enum but is out of scope here).
- A remote/CI compile service (a *pluggable-adapter* seam is left for it, but not built).
- Multi-model graph orchestration / pipelining across NPUs in one pass.

---

## 3. Locked decisions (from brainstorming)

1. **End-to-end pipeline** — one model concept across compile → package → load → run.
2. **Fat multi-backend package** (`.alpmodel`) — the source is compiled for *every backend the target SoM declares*; blobs bundled with a manifest; the runtime loader picks the matching blob at load and can switch backends on a multi-NPU SoM (V2M101 = DRP-AI3 + DEEPX).
3. **Compile-what's-available** — wrap the compilers that are present (Vela bundled; DRP-AI TVM / DEEPX wrapped if installed); record coverage in the manifest; a missing tool is a **warning**, not an error; a device whose backend isn't in the package falls back to CPU or fails clearly at load.
4. **Models declared in `board.yaml`** — a `models:` block; entries are **silicon-agnostic** (name + source only); an entry may `$ref` an external spec for large/shared models.
5. **Contract-first build order** — freeze the `.alpmodel` format first; compile-side and runtime-side then proceed in parallel against the stable contract.

---

## 4. Architecture overview

```
   board.yaml (models:)        SoM preset (inference.npu_population, preferred_backend)
            │                                 │
            ▼                                 ▼
   ┌─────────────────────────────────────────────────┐
   │  ① Compiler front-end  (Python, scripts/)        │
   │     alp model build  ──> resolves targets,       │
   │     runs CompilerAdapter per available backend   │
   └───────────────────────────┬─────────────────────┘
                                │ emits
                                ▼
   ┌─────────────────────────────────────────────────┐
   │  ② .alpmodel package  (the contract)             │
   │     header + manifest + per-backend blob sections│
   └───────────────────────────┬─────────────────────┘
              Linux: ship as-is │ MCU: extract target blob + gen C header
                                ▼
   ┌─────────────────────────────────────────────────┐
   │  ③ Runtime loader  (extends <alp/inference.h>)   │
   │     parse manifest → select blob (registry +     │
   │     preferred_backend) → existing backend ops    │
   └─────────────────────────────────────────────────┘
```

Three units, each independently testable, communicating through the `.alpmodel` manifest:

- **① Compiler front-end** — turns one source model into per-backend blobs.
- **② `.alpmodel` package + manifest** — the central, frozen contract.
- **③ Runtime loader** — selects the right blob and delegates to the existing per-backend `ops->open()`.

---

## 5. The `.alpmodel` package + manifest (the contract)

A `.alpmodel` is a **self-describing binary container** with one tiny reader usable on both Linux/A55 and Zephyr/MCU (no zip/tar dependency on-device):

```
┌────────────┬──────────────────────────┬───────────────────────────┐
│  header    │  manifest                │  blob sections             │
│  magic     │  (CBOR on-device;        │  [len][bytes] × N          │
│  "ALPM"    │   JSON is the canonical  │  (vela .tflite, drpai dir  │
│  version   │   tooling form)          │   tar, .dxnn, .tflite)     │
│  mft off/len                          │                            │
└────────────┴──────────────────────────┴───────────────────────────┘
```

**Manifest fields:**

```jsonc
{
  "alpmodel_version": 1,
  "model": {
    "name": "person_detect",
    "source_hash": "sha256:…",          // provenance + cache key
    "inputs":  [ { "dtype": "int8", "rank": 4, "shape": [1,224,224,3],
                   "scale": 0.0078, "zero_point": -1, "layout": "nhwc" } ],
    "outputs": [ { "dtype": "int8", "rank": 2, "shape": [1,1000],
                   "scale": 0.004, "zero_point": 12 } ]
  },
  "targets": [
    { "backend": "ethos_u", "silicon_ref": "alif:ensemble:e8",
      "blob_format": "vela_tflite", "accel_config": "ethos-u85-256",
      "compiler_version": "vela 4.1.0", "arena_bytes": 524288,
      "requires": { "peak_sram_kib": 512, "op_features": ["transformer"] }, "blob": 0 },
    { "backend": "drpai", "silicon_ref": "renesas:rzv2n:n44",
      "blob_format": "drpai_dir", "compiler_version": "drp-ai_tvm 2.x",
      "arena_bytes": 1048576, "requires": { "peak_sram_kib": 1024 }, "blob": 1 },
    { "backend": "cpu", "silicon_ref": "*",
      "blob_format": "tflite", "arena_bytes": 786432, "blob": 2 }
  ],
  "coverage": [
    { "backend": "ethos_u", "accel_config": "ethos-u55-128", "status": "incompatible",
      "reason": "model needs transformer ops + 512 KiB; U55-128 has neither" },
    { "backend": "deepx_dxm1", "status": "skipped", "reason": "dx-compiler not found" }
  ]
}
```

- `model.inputs/outputs` mirror the existing `alp_inference_tensor_t` shape (dtype/rank/shape[4]/scale/zero_point) so the runtime types map 1:1. Backend-independent.
- `targets[].silicon_ref` uses the **same string the registry selects on** (`alp_backend_select`), `"*"` for portable blobs.
- `backend` ids use the **metadata/preset vocabulary** (`ethos_u`, `drpai`, `deepx_dxm1`, `cpu`) — **`deepx_dxm1` is canonical** (decided 2026-05-26). The runtime maps to `alp_inference_backend_t`, and the enum `ALP_INFERENCE_BACKEND_DEEPX_DX` is renamed to `ALP_INFERENCE_BACKEND_DEEPX_DXM1` in Stage 1c — the runtime loader, which owns the enum↔string mapping (clean rename, no active customers, no compat shim).
- `targets[].arena_bytes` closes the "size it empirically" gap — the loader can size automatically.
- `targets[].requires` is the blob's **capability envelope** — peak SRAM, op-features, and (via `accel_config`) the bound NPU variant. The loader admits a blob only if the device NPU **provides** it, so a model compiled for Ethos-U85 is never mis-selected onto a small U55. **This is the capability differentiator.**
- `coverage[]` makes a partial package **explicit**, distinguishing `skipped` (compiler tool absent) from `incompatible` (the model doesn't fit that NPU variant — too big / unsupported ops on a small core).

**Two consumption modes, one format:**
- **Linux/A55** (multi-backend SoM): ship the whole `.alpmodel`; the loader picks at runtime → can switch DRP-AI3 ↔ DEEPX on V2M101.
- **MCU/Zephyr** (one NPU, flash-tight): the build **extracts the target blob** and emits a compact generated C header (`alp_model_<name>.h`: a `const` byte array + a small descriptor), consistent with the SDK's declare-in-YAML→generate-downstream pattern.

---

## 6. Model declaration (`board.yaml`)

Models attach to the project via a `models:` block in `board.yaml`. Entries are **silicon-agnostic** — name + source only:

```yaml
som:
  sku: E1M-V2M101            # → pipeline derives backends from the SoM preset

models:
  - name: person_detect
    source: models/person_detect.onnx   # ONNX or TFLite
    inputs: [{ layout: nhwc }]           # optional app-side hints only
  - name: keyword_spotter
    spec: models/kws.alpmodel.yaml       # $ref escape hatch for big/shared models
```

- **No `backend`, no `accel_config`.** The pipeline reads `som.sku` → the SoM preset's `inference.npu_population` (the set of backends the SoM physically has) and `accel_config` from the SoC `npu_population`. `preferred_backend` is the tiebreaker (see §8).
- Distinct from the existing `board.yaml` `inference: { default_arena_kib }` block (per-slice arena budget), which stays as-is.
- Schema: add `models:` to `metadata/schemas/board.schema.json`.

---

## 7. Compiler front-end + adapters

**Where:** a Python package under `scripts/` beside `alp_project.py` / `alp_orchestrate.py` — the SDK's existing "YAML → generated artifacts" home.

**The key abstraction — one `CompilerAdapter` per backend:**

```python
class CompilerAdapter(ABC):
    backend: str                      # "ethos_u" | "drpai" | "deepx_dxm1" | "cpu"

    def is_available(self) -> bool: ...        # tool present? drives compile-what's-available
    def accepts(self, src_format: str) -> bool: ...   # "onnx" | "tflite"
    def compile(self, source: Path, *, accel_config: str|None,
                out_dir: Path) -> Blob: ...    # Blob{format, payload, arena_bytes, compiler_version}
```

- **Bundled in-tree:** `vela` adapter (pip dependency, redistributable) + `cpu` passthrough (TFLite as-is).
- **Wrapper adapters (run only when `is_available()`):** `drpai` shells out to `vendors/renesas-rzv2n/rzv_drp-ai_tvm` (`compile_onnx_model.py`); `deepx` shells out to `dx-compiler`. Neither tool is redistributed by alp-sdk.

**Driver flow (`alp model build`):**
1. Read `board.yaml` → `som.sku` → SoM preset → declared backends + `accel_config`.
2. For each declared backend **and its NPU variant(s)** (from `npu_population`): tool absent → `coverage: skipped` + warn; compile attempted but the model exceeds that variant's envelope (Vela/vendor compiler rejects it, or the footprint overflows the part's SRAM) → `coverage: incompatible` + warn; success → blob + its `requires` footprint. *Compile-what's-available* thus extends to *compile-what-fits*.
3. Assemble the `.alpmodel` (manifest + blobs).
4. Emit consumption form: Linux → stage the `.alpmodel`; MCU → extract target blob + generate the C header.

**Entry points (one engine):**
- **CLI:** `alp model build [--target <sku>]` — standalone / CI-friendly (honors standalone-usage-as-first-class).
- **Declarative:** the build/orchestration calls the same engine per `models:` entry ("add YAML → package falls out").

**Caching:** keyed on `(source_hash + backend + compiler_version + accel_config)` — vendor compiles are slow.

**v1 source formats:** ONNX + TFLite. PyTorch → ONNX stays a documented pre-step.

---

## 8. Runtime loader + blob selection + errors

The loader is a **thin selection layer on top of the existing registry** — it reuses `alp_backend_select` and each backend's `inference_ops.h` `ops->open()`; it only feeds them the right blob.

**Additive API in `<alp/inference.h>`** (raw-blob `alp_inference_open` stays):

```c
typedef struct {
    const void              *data;        /* .alpmodel bytes (MCU embed) … */
    size_t                   size;        /* …or use path on Linux */
    const char              *path;        /* storage path, or NULL */
    alp_inference_backend_t  backend;     /* AUTO, or force a specific NPU */
    size_t                   arena_bytes; /* 0 = size from manifest */
    void                    *arena;
} alp_model_open_opts_t;

alp_inference_t *alp_inference_open_alpmodel(const alp_model_open_opts_t *opts);
/* returns the same alp_inference_t; num_inputs/get_input/invoke/close all work unchanged */
```

**Selection algorithm:**
1. Parse header + manifest.
2. Read `ALP_SOC_REF_STR` (from `<alp/soc_caps.h>`) and which inference backends are registered/compiled-in for this SoM (`alp_backend_count`/registry walk).
3. Find `targets[]` where `backend` is available **and** `silicon_ref` is compatible **and** the device NPU **satisfies** `requires` — the bound variant matches `accel_config`, available SRAM ≥ `peak_sram_kib`, and the op-features are supported. (This is where a big model is rejected from a small U55.)
4. Among matches, pick by registry **priority**, with the SoM preset's **`preferred_backend` as the tiebreaker** — this finally makes `preferred_backend` *do something* (it is informational today).
5. Delegate the chosen blob + its `arena_bytes` to that backend's existing `ops->open()`.

**App override:** `opts.backend = ALP_INFERENCE_BACKEND_DEEPX_DX` forces a specific NPU on a multi-backend SoM; errors if the package has no such blob.

**Error handling (explicit — no silent failures):**
- No matching blob for any available backend → fall back to a `cpu` blob if present; else `ALP_ERR_NO_BACKEND` (new code or nearest existing) naming *package coverage vs device backends*.
- Model doesn't **fit** any available NPU (compiled for a larger core than the device has — e.g. a U85 model on a U55-128) → CPU fallback if present; else `ALP_ERR_NO_FIT` stating the model's `requires` vs the device NPU's envelope.
- Explicit backend requested but absent → `ALP_ERR_NOT_FOUND` ("requested deepx_dx; package has {ethos_u, cpu}").
- `alpmodel_version` > loader → `ALP_ERR_VERSION`. Bad magic / corrupt blob → `ALP_ERR_INVAL`.
- Caller arena smaller than `manifest.arena_bytes` → error stating the required size; `arena_bytes == 0` → loader sizes from the manifest.

**Capabilities:** `alp_inference_capabilities()` (exists) gets populated from the manifest + backend `probe`, so apps can finally query the active backend's traits.

---

## 9. Build order (contract-first)

Each stage builds on a **frozen** `.alpmodel` contract, so compile-side and runtime-side proceed in parallel after Stage 1.

- **Stage 1 — contract + walking skeleton (Vela + CPU).** Lock the `.alpmodel` container/manifest (JSON canonical + on-device reader); front-end skeleton (`CompilerAdapter` + `vela` + `cpu`, `alp model build` CLI, `board.yaml models:` → target derivation); runtime loader + selection wired to the **existing** Ethos-U + CPU/TFLM backends. **Exit:** compile → package → load → run works on Ethos-U (AEN/i.MX93) + CPU.
  - **STATUS 2026-05-26 — Stage 1a (the `.alpmodel` contract) IMPLEMENTED** on `feat/unified-model-pipeline`: `scripts/alp_model/` host writer (manifest + canonical JSON + CBOR + binary container + fixture generator) and `include/alp/model.h` + `src/common/alp_model.c` on-device `zcbor` reader (compiled behind Kconfig `ALP_SDK_MODEL_READER`, which `select`s `ZCBOR`). Round-trip proven: pytest 8/8 (host) + native_sim ztest 4/4 (incl. bad-magic / bad-version / truncated-table rejection). **Remaining for Stage 1:** the compiler front-end **(1b)** and the runtime loader/selection **(1c)** — each its own plan.
- **Stage 2 — proprietary adapters + finish those backends.** `drpai` adapter (wraps `rzv_drp-ai_tvm`) + turn `drpai_v2n_stub.c` into a real backend; `deepx` adapter + `deepx_dxm1_stub.c` → real. They slot into the frozen contract with **no format change**. Gated on the proprietary tools + bench HW.
- **Stage 3 — integration polish.** Declarative build hook (orchestration calls the engine per `models:` entry; MCU C-header emit; Linux image staging); compile caching; capability population; coverage warnings; docs + one example.

---

## 10. Testing strategy

All host-runnable gates wired into the existing twister/native_sim + pytest setup and run **local-first on Windows + WSL** before push.

- **Manifest** — Python unit tests for the writer + a **C round-trip** test (write package in Python → parse with the on-device reader). native_sim, no HW.
- **Selection logic** (highest bug density → most coverage) — C unit tests with synthetic manifests + faked `ALP_SOC_REF_STR`/registered backends, hitting every branch: single match, priority, `preferred_backend` tiebreak, app override, no-backend, CPU fallback, version/arena/corrupt errors.
- **Adapters** — `is_available()` / `accepts()` unit-tested; `compile()` against a tiny fixture model where the tool exists (Vela in CI; drpai/deepx mocked/skipped when absent — mirrors compile-what's-available).
- **End-to-end** — a small MobileNet built through the CLI in CI for Vela + CPU. Real NPU runs (DRP-AI/DEEPX/Ethos on silicon) stay **HIL/bench-gated**, consistent with `docs/test-plan.md`'s untested rows.

---

## 11. Existing code this builds on / touches

| Area | File(s) | Change |
|------|---------|--------|
| Runtime API | `include/alp/inference.h` | **Add** `alp_inference_open_alpmodel` + `alp_model_open_opts_t` (additive; raw-blob open stays) |
| Dispatch | `src/inference_dispatch.c` | Add `.alpmodel` parse + selection in front of the existing `ops->open` delegation |
| Backend ops | `src/backends/inference/inference_ops.h` | Reused unchanged (loader feeds chosen blob) |
| Stubs → real | `src/backends/inference/drpai_v2n_stub.c`, `deepx_dxm1_stub.c` | Stage 2 |
| Registry | `include/alp/backend.h` | Reused (`alp_backend_select`, `ALP_SOC_REF_STR`) |
| Front-end | `scripts/` (new package) | New `alp model` CLI + `CompilerAdapter`s |
| Metadata | `metadata/schemas/board.schema.json` | **Add** `models:` block |
| Metadata | `metadata/e1m_modules/<SKU>.yaml` | Already carry `inference.{preferred_backend,npu_population}` — now *consumed* |
| Vendor tool | `vendors/renesas-rzv2n/rzv_drp-ai_tvm/` | Wrapped by the `drpai` adapter |
| Docs | `docs/tutorials/16-inference-mobilenet.md`, `docs/test-plan.md`, `docs/os-support-matrix.md` | Update once stages land |

---

## 12. Open questions / future work

- **Backend id spelling** — *resolved 2026-05-26:* canonical is `deepx_dxm1`; the `ALP_INFERENCE_BACKEND_DEEPX_DX` enum is renamed to `ALP_INFERENCE_BACKEND_DEEPX_DXM1` in Stage 1c (the runtime loader; Stage 1a only needs the canonical *string* `deepx_dxm1`, which is already used consistently).
- **On-device manifest encoding** — *resolved 2026-05-26:* **CBOR** (`zcbor` on-device, a Python CBOR lib on host; JSON kept as the canonical tooling form). Chosen for extensibility + alignment with the Zephyr/SUIT/MCUboot firmware-manifest lane (`zcbor`) and a future **COSE-signing** path that matches the SDK's secure-boot/OTA trust model.
- **`.alpmodel` container framing** — exact header layout + whether blob sections are aligned for `mmap` on Linux.
- **Pluggable remote-compile adapter** — leave the `CompilerAdapter` seam so a CI/remote builder holding licensed toolchains can produce complete packages later (deferred).
- **Quantization / calibration** — explicitly out of v1; revisit if a generic PTQ front-end is wanted.
- **Capability envelope source** — the per-variant NPU limits (MACs / SRAM / op-set) that `requires` is checked against should come from the SoC JSON `npu_population`; confirm those fields exist or extend `soc-spec-v1.schema.json` in Stage 1.
