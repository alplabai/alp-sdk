# `alp model check` — Pre-flight Fit/Perf Analyzer (Slice 1a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `alp model check <model> --sku <SKU>` — an offline, no-toolchain static analyzer that answers "will this model fit my SoM's NPU, and roughly how fast?" before any compile.

**Architecture:** A new pure engine `scripts/alp_model/analyze.py` composes existing SoM→backend resolution (`targets.resolve_targets`) with a new TFLite operator walk (`tensorio.extract_ops`) and a new manufacturer-owned per-NPU op-support data asset (`metadata/npu_ops/<backend>.json`). It emits a per-backend fit verdict + conservative SRAM/latency/op-coverage estimates, all labelled `source: static`. A thin `check_cmd` in `scripts/alp_cli/model.py` surfaces it with the existing `--format {human,json}` convention.

**Tech Stack:** Python 3.11 (`py -3.11`), `click` CLI, the `tflite` flatbuffer reader (the `model-io` extra — `pip install "tflite>=2.10" "flatbuffers>=23.1.21"`), `pytest` + `click.testing.CliRunner`.

## Global Constraints

- **Fidelity tier 1 only.** This slice ships the *static* estimator. Tier 2 (manufacturer-precomputed bench data) and tier 3 (exact-on-demand toolchain) are follow-ons. Every verdict this slice emits carries `source: "static"`.
- **Never over-promise "fits."** Bias every estimate conservative: round SRAM up, treat unknown/unsupported ops as CPU-fallback (never silently "fits"), assume ≤50% NPU utilization for latency (pessimistic). A false "fits" churns a customer — it is the worst failure.
- **TFLite input only in 1a.** ONNX static analysis is the immediate follow-on. A non-`.tflite` model must produce a clear "not supported yet" error, never a fabricated verdict.
- **SRAM verdict is dormant but displayed.** Every `inference_arena_sram_kib` in `metadata/socs/**` is currently `0` (= "unknown/unbounded, fit-check skips"). So `no-fit` from SRAM overflow cannot fire yet; the analyzer still *displays* `est_sram_kib` and honours a non-zero budget the day one is authored. The in-family cross-sell suggestion (next-bigger SoM) is SRAM-driven → also dormant → a labelled follow-on. Do not build the SoM-ordering catalog in 1a.
- **Op-support lists are seed data.** Conservative subsets from public vendor docs (Vela's TFLite allowlist / Renesas DRP-AI / DEEPX), each carrying a `source` note + `version`. They are the manufacturer-owned moat, refined from bench probing later — labelled seed, not fabricated precision.
- **Backend vocabulary** (from `targets._npu_backend`): `cpu`, `ethos_u`, `drpai`, `deepx_dxm1`. `cpu` is the universal fallback (supports everything, always `fits`, no arena limit).
- **Verdict enum:** `fits` | `cpu-fallback` | `no-fit`.
- **Public repo hygiene:** no secrets, no login-gated vendor download links, no local absolute paths in committed files, no SoM design leaks. Company name "Alp Lab". No Claude/AI attribution in commits — attribute to alpCaner.
- **This branch:** `feat/alp-model-check`, stacked on `feat/alp-model-envelope` (#907, which adds the `alp model` list/doctor/info/build subcommands this slice extends). Do NOT merge. PR targets `dev`.
- **Local gate before any commit that touches `metadata/` or schemas:** `py -3.11 scripts/validate_metadata.py`. Before any Python commit: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py tests/scripts/test_alp_model_tensorio.py tests/scripts/test_alp_cli_model.py -q`.

---

## File Structure

- **Create** `scripts/alp_model/analyze.py` — the pure analyzer engine (estimators + `analyze_model` + result dataclasses + `UnsupportedModelError`). One responsibility: turn (model ops, SoM spec, op-support) into a fit/perf verdict. No `click`, no CLI.
- **Modify** `scripts/alp_model/tensorio.py` — add `extract_ops()` + `OpDesc`/`TensorDesc` dataclasses (the TFLite operator walk). All flatbuffer decoding stays here, co-located with the existing `extract_io`.
- **Create** `metadata/npu_ops/ethos_u.json`, `metadata/npu_ops/drpai.json`, `metadata/npu_ops/deepx_dxm1.json` — the per-NPU op-support data asset.
- **Create** `metadata/schemas/npu-ops-v1.schema.json` — schema for the op-support files.
- **Modify** `scripts/validate_metadata.py` — validate `metadata/npu_ops/*.json` against the new schema (good-citizen: the data asset is versioned + gated).
- **Modify** `scripts/alp_cli/model.py` — add `check_cmd` (`@model_group.command(name="check")`).
- **Create** `tests/scripts/test_alp_model_analyze.py` — engine unit tests (synthetic ops → exact estimator math) + `analyze_model` integration (real SKU → structural invariants).
- **Modify** `tests/scripts/test_alp_model_tensorio.py` — add `extract_ops` coverage.
- **Modify** `tests/scripts/test_alp_cli_model.py` — add `model check` CLI coverage (human + json + non-tflite rejection).

---

## Task 1: TFLite operator walk (`tensorio.extract_ops`)

**Files:**
- Modify: `scripts/alp_model/tensorio.py`
- Test: `tests/scripts/test_alp_model_tensorio.py`

**Interfaces:**
- Consumes: the installed `tflite` flatbuffer reader (same as `extract_io`).
- Produces:
  - `TensorDesc(shape: list[int], dtype: str, nbytes: int, is_const: bool)` — frozen dataclass.
  - `OpDesc(op: str, inputs: list[TensorDesc], outputs: list[TensorDesc])` — frozen dataclass. `op` is the TFLite builtin name (e.g. `"CONV_2D"`, `"FULLY_CONNECTED"`), or `"OP_<n>"` when a code is unmapped.
  - `extract_ops(source: Path, *, raw: bytes | None = None) -> list[OpDesc]` — returns `[]` for a non-`.tflite` source, a missing `tflite` parser, or unparseable bytes (mirrors `extract_io`'s best-effort contract — never raises).

- [ ] **Step 1: Write the failing tests**

Add to `tests/scripts/test_alp_model_tensorio.py`:

```python
def test_extract_ops_non_tflite_returns_empty(tmp_path):
    from alp_model.tensorio import extract_ops
    src = tmp_path / "m.onnx"
    src.write_bytes(b"not a tflite model")
    assert extract_ops(src) == []


def test_extract_ops_walks_fixture_operators():
    pytest.importorskip("tflite")
    from alp_model.tensorio import extract_ops
    ops = extract_ops(_FIXTURE)              # tests/fixtures/models/tiny_int8.tflite
    assert len(ops) == 1
    fc = ops[0]
    assert fc.op == "FULLY_CONNECTED"
    # activation input [1,4] int8 (not const) + weight [2,4] int8 (const)
    act = [t for t in fc.inputs if not t.is_const]
    wts = [t for t in fc.inputs if t.is_const]
    assert [t.shape for t in act] == [[1, 4]]
    assert act[0].dtype == "int8" and act[0].nbytes == 4
    assert any(t.shape == [2, 4] for t in wts)          # weights present, flagged const
    assert fc.outputs[0].shape == [1, 2] and fc.outputs[0].nbytes == 2
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /e/GitHub/alp-sdk/.claude/worktrees/model-check && py -3.11 -m pytest tests/scripts/test_alp_model_tensorio.py -q`
Expected: FAIL — `ImportError: cannot import name 'extract_ops'`.

- [ ] **Step 3: Implement `extract_ops` + the dataclasses**

In `scripts/alp_model/tensorio.py`, add the `dataclass` import and the new code (keep `extract_io`/`_describe` unchanged):

```python
from dataclasses import dataclass

# Byte width per SDK dtype name (the tensorio dtype vocabulary). Unknown -> 4
# (conservative: never under-count a tensor's arena footprint).
_DTYPE_BYTES = {"f32": 4, "f16": 2, "int32": 4, "int16": 2, "uint8": 1, "int8": 1}


@dataclass(frozen=True)
class TensorDesc:
    shape: list[int]
    dtype: str
    nbytes: int
    is_const: bool          # True = weight/bias (lives in flash), excluded from arena


@dataclass(frozen=True)
class OpDesc:
    op: str                 # TFLite builtin name, e.g. "CONV_2D"; "OP_<n>" if unmapped
    inputs: list[TensorDesc]
    outputs: list[TensorDesc]


def extract_ops(source: Path, *, raw: bytes | None = None) -> list[OpDesc]:
    """Walk a TFLite subgraph's operators for the static analyzer. Best-effort:
    returns [] for a non-.tflite source, a missing `tflite` reader, or bytes that
    don't parse -- the analyzer treats an empty walk as 'not statically analysable'
    rather than fabricating a verdict."""
    if source.suffix.lower() != ".tflite":
        return []
    try:
        import tflite
    except ImportError:
        return []
    if raw is None:
        raw = source.read_bytes()
    try:
        model = tflite.Model.GetRootAs(raw, 0)
        if model.SubgraphsLength() == 0:
            return []
        g = model.Subgraphs(0)
        dtype_map = {
            tflite.TensorType.FLOAT32: "f32", tflite.TensorType.FLOAT16: "f16",
            tflite.TensorType.INT32: "int32", tflite.TensorType.UINT8: "uint8",
            tflite.TensorType.INT16: "int16", tflite.TensorType.INT8: "int8",
        }
        names = {v: k for k, v in vars(tflite.BuiltinOperator).items()
                 if isinstance(v, int)}

        def _td(idx: int) -> TensorDesc:
            t = g.Tensors(idx)
            shape = [int(t.Shape(k)) for k in range(t.ShapeLength())]
            dtype = dtype_map.get(t.Type(), f"tflite:{t.Type()}")
            nbytes = _DTYPE_BYTES.get(dtype, 4)
            for dim in shape:
                nbytes *= dim               # scalar/empty shape -> stays one element
            buf_idx = t.Buffer()
            buf = model.Buffers(buf_idx) if buf_idx >= 0 else None
            is_const = bool(buf is not None and buf.DataLength() > 0)
            return TensorDesc(shape=shape, dtype=dtype, nbytes=nbytes, is_const=is_const)

        ops: list[OpDesc] = []
        for i in range(g.OperatorsLength()):
            op = g.Operators(i)
            oc = model.OperatorCodes(op.OpcodeIndex())
            code = max(oc.BuiltinCode(), oc.DeprecatedBuiltinCode())
            ins = [_td(op.Inputs(j)) for j in range(op.InputsLength()) if op.Inputs(j) >= 0]
            outs = [_td(op.Outputs(j)) for j in range(op.OutputsLength()) if op.Outputs(j) >= 0]
            ops.append(OpDesc(op=names.get(code, f"OP_{code}"), inputs=ins, outputs=outs))
        return ops
    except Exception:
        return []
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_tensorio.py -q`
Expected: PASS (existing `extract_io` tests + the two new `extract_ops` tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/tensorio.py tests/scripts/test_alp_model_tensorio.py
git commit -m "feat(model): walk TFLite operators for the static analyzer (extract_ops)"
```

---

## Task 2: Per-NPU op-support data asset + schema + validator wiring

**Files:**
- Create: `metadata/npu_ops/ethos_u.json`
- Create: `metadata/npu_ops/drpai.json`
- Create: `metadata/npu_ops/deepx_dxm1.json`
- Create: `metadata/schemas/npu-ops-v1.schema.json`
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_alp_model_analyze.py` (new file — a data-integrity test lands here; the engine tests follow in Task 3)

**Interfaces:**
- Produces: `metadata/npu_ops/<backend>.json` files, each `{"backend", "version", "source", "supported_ops": [TFLite builtin names]}`. Loaded in Task 3 by `analyze._load_op_support(backend, metadata_root)`.

- [ ] **Step 1: Write the failing data-integrity test**

Create `tests/scripts/test_alp_model_analyze.py`:

```python
"""alp_model.analyze — static fit/perf analyzer."""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"
_FIXTURE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
_ONNX_FIXTURE = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


@pytest.mark.parametrize("backend", ["ethos_u", "drpai", "deepx_dxm1"])
def test_op_support_file_shape(backend):
    data = json.loads((_META / "npu_ops" / f"{backend}.json").read_text("utf-8"))
    assert data["backend"] == backend
    assert data["version"] and data["source"]
    assert isinstance(data["supported_ops"], list) and data["supported_ops"]
    # Op names are TFLite builtin identifiers (UPPER_SNAKE), deduped
    assert all(op == op.upper() for op in data["supported_ops"])
    assert len(data["supported_ops"]) == len(set(data["supported_ops"]))
    # Every NPU must at least run the compute-dominant ops the estimator scores
    assert {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"} <= set(data["supported_ops"])
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py -q`
Expected: FAIL — `FileNotFoundError: metadata/npu_ops/ethos_u.json`.

- [ ] **Step 3: Create the op-support data files**

`metadata/npu_ops/ethos_u.json` (conservative subset of Vela's supported TFLite builtin ops):

```json
{
  "backend": "ethos_u",
  "version": "2026.07-seed",
  "source": "Conservative subset of Ethos-U Vela SUPPORTED_OPS (public docs); refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "TRANSPOSE_CONV", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D", "MEAN",
    "ADD", "MUL", "SUB", "CONCATENATION", "PAD", "RESHAPE",
    "SOFTMAX", "LOGISTIC", "TANH", "RELU", "RELU6", "LEAKY_RELU", "HARD_SWISH",
    "RESIZE_BILINEAR", "RESIZE_NEAREST_NEIGHBOR",
    "SPLIT", "STRIDED_SLICE", "QUANTIZE", "DEQUANTIZE"
  ]
}
```

`metadata/npu_ops/drpai.json`:

```json
{
  "backend": "drpai",
  "version": "2026.07-seed",
  "source": "Conservative subset from Renesas DRP-AI TVM supported-op docs; refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D",
    "ADD", "MUL", "CONCATENATION", "PAD", "RESHAPE",
    "SOFTMAX", "RELU", "RELU6", "RESIZE_BILINEAR"
  ]
}
```

`metadata/npu_ops/deepx_dxm1.json`:

```json
{
  "backend": "deepx_dxm1",
  "version": "2026.07-seed",
  "source": "Conservative subset from DEEPX DX-M1 CNN-accelerator docs; refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D",
    "ADD", "CONCATENATION", "RESHAPE",
    "SOFTMAX", "RELU", "RELU6"
  ]
}
```

- [ ] **Step 4: Create the schema**

`metadata/schemas/npu-ops-v1.schema.json`:

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://alplab.ai/schemas/npu-ops-v1.schema.json",
  "title": "ALP per-NPU op-support list v1",
  "type": "object",
  "additionalProperties": false,
  "required": ["backend", "version", "source", "supported_ops"],
  "properties": {
    "backend": {"type": "string", "enum": ["ethos_u", "drpai", "deepx_dxm1"]},
    "version": {"type": "string", "minLength": 1},
    "source": {"type": "string", "minLength": 1},
    "supported_ops": {
      "type": "array",
      "minItems": 1,
      "uniqueItems": true,
      "items": {"type": "string", "pattern": "^[A-Z0-9_]+$"}
    }
  }
}
```

- [ ] **Step 5: Wire the validator**

Read `scripts/validate_metadata.py` to find where it validates the `socs`/`e1m_modules`/`boards` globs against their schemas, and add an analogous block. The exact call shape must match the file's existing helper (it already loads schemas and iterates a glob); mirror that pattern. The addition validates every `metadata/npu_ops/*.json` against `metadata/schemas/npu-ops-v1.schema.json`. Concretely, alongside the existing per-directory validation loops, add:

```python
    # Per-NPU op-support lists (the static-analyzer data asset)
    npu_ops_schema = load_schema(SCHEMAS / "npu-ops-v1.schema.json")
    for path in sorted((METADATA / "npu_ops").glob("*.json")):
        validate_file(path, npu_ops_schema, errors)
```

Use whatever the file's real symbol names are for `load_schema`, `validate_file`, `errors`, `METADATA`, and `SCHEMAS` — match them exactly to the existing loops in that file.

- [ ] **Step 6: Run the data-integrity test + the metadata gate**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py -q`
Expected: PASS (3 parametrized cases).

Run: `py -3.11 scripts/validate_metadata.py`
Expected: exit 0, the `npu_ops` files reported valid (no schema errors).

- [ ] **Step 7: Commit**

```bash
git add metadata/npu_ops/ metadata/schemas/npu-ops-v1.schema.json scripts/validate_metadata.py tests/scripts/test_alp_model_analyze.py
git commit -m "feat(metadata): seed per-NPU op-support lists + schema for the static analyzer"
```

---

## Task 3: The analyzer engine (`analyze.py`)

**Files:**
- Create: `scripts/alp_model/analyze.py`
- Test: `tests/scripts/test_alp_model_analyze.py` (extend)

**Interfaces:**
- Consumes: `tensorio.extract_ops` + `OpDesc`/`TensorDesc` (Task 1); `targets.resolve_targets` + `targets._npu_backend` + `targets.TargetSpec` (existing); the `metadata/npu_ops/*.json` files (Task 2); `inference_arena_sram_kib` + `npus[]` from `metadata/socs/**/*.json` (existing).
- Produces:
  - `BackendResult(backend, verdict, est_sram_kib, budget_sram_kib, est_latency_ms, op_coverage_pct, unsupported_ops, source)` — frozen dataclass.
  - `AnalysisResult(model, sku, backends: list[BackendResult], suggestion: str | None)` — frozen dataclass.
  - `class UnsupportedModelError(Exception)`.
  - `analyze_model(model_path: Path, sku: str, *, metadata_root: Path) -> AnalysisResult`.
  - Pure estimator helpers: `estimate_macs`, `estimate_peak_sram_kib`, `estimate_latency_ms`, `op_coverage`, `npu_mac_per_s`.

- [ ] **Step 1: Write the failing estimator unit tests**

Extend `tests/scripts/test_alp_model_analyze.py`:

```python
def _op(name, inputs, outputs):
    from alp_model.tensorio import OpDesc, TensorDesc
    def td(shape, const=False, dtype="int8", nb=None):
        n = (nb if nb is not None else max(1, __import__("math").prod(shape)))
        return TensorDesc(shape=list(shape), dtype=dtype, nbytes=n, is_const=const)
    return OpDesc(op=name,
                  inputs=[td(*i) if isinstance(i, tuple) else td(i) for i in inputs],
                  outputs=[td(o) for o in outputs])


def test_estimate_macs_fully_connected():
    from alp_model.analyze import estimate_macs
    # activation [1,4], weights [2,4] const -> 1 * units(2) * in_f(4) = 8
    op = _op("FULLY_CONNECTED", [([1, 4],), ([2, 4], True)], [[1, 2]])
    assert estimate_macs([op]) == 8


def test_estimate_macs_conv2d():
    from alp_model.analyze import estimate_macs
    # out [1,8,8,16], weights [16,3,3,4] -> 8*8*16*3*3*4 = 368640
    op = _op("CONV_2D", [([1, 10, 10, 4],), ([16, 3, 3, 4], True)], [[1, 8, 8, 16]])
    assert estimate_macs([op]) == 368640


def test_estimate_macs_unknown_op_is_zero():
    from alp_model.analyze import estimate_macs
    assert estimate_macs([_op("CUSTOM_THING", [[1, 4]], [[1, 4]])]) == 0


def test_estimate_peak_sram_excludes_constants_and_rounds_up():
    from alp_model.analyze import estimate_peak_sram_kib
    # activations: in 4B + out 2B = 6B -> ceil(6/1024) = 1 KiB; weights (const) excluded
    op = _op("FULLY_CONNECTED", [([1, 4], False, "int8", 4), ([2, 4], True, "int8", 8)],
             [[1, 2]])
    # give the output an explicit 2-byte size
    from alp_model.tensorio import OpDesc, TensorDesc
    op = OpDesc(op="FULLY_CONNECTED",
                inputs=[TensorDesc([1, 4], "int8", 4, False),
                        TensorDesc([2, 4], "int8", 8, True)],
                outputs=[TensorDesc([1, 2], "int8", 2, False)])
    assert estimate_peak_sram_kib([op]) == 1


def test_op_coverage_partial():
    from alp_model.analyze import op_coverage
    ops = [_op("CONV_2D", [[1, 4]], [[1, 4]]), _op("CUSTOM_X", [[1, 4]], [[1, 4]])]
    pct, unsupported = op_coverage(ops, {"CONV_2D"})
    assert pct == 50.0 and unsupported == ["CUSTOM_X"]


def test_latency_prefers_mac_per_cycle_then_tops():
    from alp_model.analyze import npu_mac_per_s
    assert npu_mac_per_s({"mac_per_cycle": 256, "freq_mhz": 400}) == 256 * 400e6
    assert npu_mac_per_s({"tops": 4}) == 4e12 / 2
    assert npu_mac_per_s({"gops": 200}) == 200e9 / 2
    assert npu_mac_per_s({}) is None
```

- [ ] **Step 2: Run to verify failure**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_model.analyze'`.

- [ ] **Step 3: Implement `analyze.py`**

Create `scripts/alp_model/analyze.py`:

```python
# scripts/alp_model/analyze.py
"""Static pre-flight fit/perf analyzer (fidelity tier 1).

Offline, no NPU toolchain: parse a TFLite graph, and for each backend a SoM
exposes, estimate op-coverage, peak arena SRAM, and rough latency -> a fit
verdict. Every number is a conservative estimate labelled `source: static`;
the tier-2 bench data (a follow-on) will calibrate these toward reality.

Correctness rule: NEVER over-promise 'fits'. Round SRAM up, treat unknown ops
as CPU-fallback, assume <=50% NPU utilisation for latency."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from .targets import _npu_backend, resolve_targets
from .tensorio import OpDesc, extract_ops

# Assume 50% NPU utilisation -> latency is pessimistic (higher). A naive roofline;
# tier-2 bench measurement replaces this per-NPU factor with a fitted model.
_UTIL = 0.5


class UnsupportedModelError(Exception):
    """The model can't be statically analysed (non-TFLite, or unparseable) --
    raised instead of emitting a fabricated verdict."""


@dataclass(frozen=True)
class BackendResult:
    backend: str
    verdict: str                       # fits | cpu-fallback | no-fit
    est_sram_kib: int
    budget_sram_kib: int | None        # None = unknown/unbounded (arena not authored)
    est_latency_ms: float | None       # None = no throughput data (e.g. cpu backend)
    op_coverage_pct: float
    unsupported_ops: list[str]
    source: str = "static"


@dataclass(frozen=True)
class AnalysisResult:
    model: str
    sku: str
    backends: list[BackendResult] = field(default_factory=list)
    suggestion: str | None = None


# --- pure estimators ------------------------------------------------------

def _op_macs(od: OpDesc) -> int:
    outs = od.outputs
    if not outs:
        return 0
    out = outs[0].shape
    name = od.op
    if name == "FULLY_CONNECTED":
        # weights = inputs[1] [units, in_features]; batch = product of out[:-1]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 2:
            units, in_f = od.inputs[1].shape
            batch = 1
            for d in out[:-1]:
                batch *= d
            return batch * units * in_f
        return 0
    if name in ("CONV_2D", "TRANSPOSE_CONV"):
        # weights [c_out,kh,kw,c_in]; out [n,oh,ow,c_out]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 4 and len(out) == 4:
            c_out, kh, kw, c_in = od.inputs[1].shape
            _, oh, ow, _ = out
            return oh * ow * c_out * kh * kw * c_in
        return 0
    if name == "DEPTHWISE_CONV_2D":
        # weights [1,kh,kw,c]; out [n,oh,ow,c]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 4 and len(out) == 4:
            _, kh, kw, c = od.inputs[1].shape
            _, oh, ow, _ = out
            return oh * ow * c * kh * kw
        return 0
    return 0                            # elementwise/unknown -> negligible MACs


def estimate_macs(ops: list[OpDesc]) -> int:
    return sum(_op_macs(od) for od in ops)


def estimate_peak_sram_kib(ops: list[OpDesc]) -> int:
    """Conservative upper bound: sum every activation-tensor occurrence (a tensor
    counted once as a producer's output and again as a consumer's input -> a
    deliberate over-count so a static check never falsely reports 'fits').
    Constants (weights/bias) live in flash and are excluded. Rounds up to KiB.
    ponytail: sum-of-activations upper bound; swap for a true liveness high-water
    once tier-2 bench SRAM calibrates it."""
    total = sum(t.nbytes for od in ops for t in (*od.inputs, *od.outputs)
                if not t.is_const)
    return (total + 1023) // 1024


def npu_mac_per_s(npu: dict) -> float | None:
    mpc, fmhz = npu.get("mac_per_cycle"), npu.get("freq_mhz")
    if mpc and fmhz:
        return mpc * fmhz * 1e6
    if npu.get("tops"):
        return npu["tops"] * 1e12 / 2       # 1 MAC = 2 ops
    if npu.get("gops"):
        return npu["gops"] * 1e9 / 2
    return None


def estimate_latency_ms(macs: int, npu: dict) -> float | None:
    rate = npu_mac_per_s(npu)
    if not rate:
        return None
    return macs / (rate * _UTIL) * 1e3


def op_coverage(ops: list[OpDesc], supported: set[str]) -> tuple[float, list[str]]:
    if not ops:
        return 100.0, []
    covered = sum(1 for od in ops if od.op in supported)
    unsupported = sorted({od.op for od in ops if od.op not in supported})
    return round(covered / len(ops) * 100, 1), unsupported


# --- metadata readers -----------------------------------------------------

def _load_op_support(backend: str, metadata_root: Path) -> set[str]:
    p = metadata_root / "npu_ops" / f"{backend}.json"
    if not p.is_file():
        return set()                        # no data -> everything unsupported (safe)
    return set(json.loads(p.read_text("utf-8")).get("supported_ops", []))


def _load_npu(spec, metadata_root: Path) -> tuple[dict | None, int]:
    """Re-read the SoC spec named by the resolved target's silicon_ref, and return
    (matching npu dict, arena KiB). Prefers the npu whose accel-config matches the
    target (a SoC may expose two Ethos-U configs)."""
    vendor, family, variant = spec.silicon_ref.split(":")
    soc = json.loads(
        (metadata_root / "socs" / vendor / family / f"{variant}.json").read_text("utf-8"))
    arena = int(soc.get("inference_arena_sram_kib", 0))
    match = None
    for npu in soc.get("npus", []):
        if _npu_backend(npu.get("type", ""), npu.get("subtype", "")) != spec.backend:
            continue
        match = match or npu
        if spec.accel_config and \
                f"{npu.get('type', '')}-{npu.get('mac_per_cycle')}" == spec.accel_config:
            return npu, arena               # exact accel-config match wins
    return match, arena


def _suggest(backends: list[BackendResult]) -> str | None:
    for b in backends:
        if b.backend == "cpu":
            continue
        if b.verdict == "no-fit":
            return (f"{b.backend}: arena overflow (~{b.est_sram_kib} KiB > "
                    f"{b.budget_sram_kib} KiB budget) -- quantize/prune, or move to a "
                    f"larger in-family SoM.")
        if b.verdict == "cpu-fallback" and b.unsupported_ops:
            return (f"{b.backend}: {len(b.unsupported_ops)} op(s) fall back to CPU "
                    f"({', '.join(b.unsupported_ops)}) -- substitute NPU-friendly ops "
                    f"or re-quantize.")
    return None


def analyze_model(model_path: Path, sku: str, *, metadata_root: Path) -> AnalysisResult:
    ops = extract_ops(model_path)
    if not ops:
        raise UnsupportedModelError(
            f"static check supports .tflite models in this release; cannot analyse "
            f"{model_path.name} (ONNX static analysis is a follow-on)")

    macs = estimate_macs(ops)
    sram_kib = estimate_peak_sram_kib(ops)
    backends: list[BackendResult] = []

    for spec in resolve_targets(sku, metadata_root=metadata_root):
        if spec.backend == "cpu":
            backends.append(BackendResult(
                backend="cpu", verdict="fits", est_sram_kib=sram_kib,
                budget_sram_kib=None, est_latency_ms=None,
                op_coverage_pct=100.0, unsupported_ops=[]))
            continue
        npu, arena = _load_npu(spec, metadata_root)
        supported = _load_op_support(spec.backend, metadata_root)
        cov, unsupported = op_coverage(ops, supported)
        latency = estimate_latency_ms(macs, npu) if npu else None
        if arena and sram_kib > arena:
            verdict = "no-fit"              # SRAM overflow (dormant until arenas authored)
        elif unsupported:
            verdict = "cpu-fallback"        # unknown/unsupported op -> CPU (never fake-fit)
        else:
            verdict = "fits"
        backends.append(BackendResult(
            backend=spec.backend, verdict=verdict, est_sram_kib=sram_kib,
            budget_sram_kib=(arena or None), est_latency_ms=latency,
            op_coverage_pct=cov, unsupported_ops=unsupported))

    return AnalysisResult(model=str(model_path), sku=sku, backends=backends,
                          suggestion=_suggest(backends))
```

- [ ] **Step 4: Run the estimator unit tests**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py -q`
Expected: PASS (data-integrity + all estimator tests).

- [ ] **Step 5: Write the `analyze_model` integration tests**

Extend `tests/scripts/test_alp_model_analyze.py`:

```python
def test_analyze_model_tflite_structural_invariants():
    pytest.importorskip("tflite")
    from alp_model.analyze import analyze_model
    res = analyze_model(_FIXTURE, "E1M-AEN801", metadata_root=_META)
    assert res.sku == "E1M-AEN801"
    assert res.backends, "expected at least one backend"
    # cpu is the universal fallback: always present, always fits, source static
    cpu = [b for b in res.backends if b.backend == "cpu"]
    assert len(cpu) == 1 and cpu[0].verdict == "fits"
    assert all(b.source == "static" for b in res.backends)
    # AEN701 -> alif e7 exposes an Ethos-U backend
    assert any(b.backend == "ethos_u" for b in res.backends)
    # tiny FC model, all-supported, budget unknown -> ethos verdict never no-fit
    for b in res.backends:
        if b.backend == "ethos_u":
            assert b.verdict in ("fits", "cpu-fallback")
            assert b.est_sram_kib == 1        # 6B activations -> 1 KiB
            assert b.budget_sram_kib is None   # e7 arena is a 0 placeholder


def test_analyze_model_rejects_non_tflite():
    from alp_model.analyze import analyze_model, UnsupportedModelError
    with pytest.raises(UnsupportedModelError):
        analyze_model(_ONNX_FIXTURE, "E1M-AEN801", metadata_root=_META)


def test_analyze_model_drpai_backend_present():
    pytest.importorskip("tflite")
    from alp_model.analyze import analyze_model
    res = analyze_model(_FIXTURE, "E1M-V2N101", metadata_root=_META)
    assert any(b.backend == "drpai" for b in res.backends)
```

- [ ] **Step 6: Run the full engine suite**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_analyze.py -q`
Expected: PASS (all data + estimator + integration tests).

- [ ] **Step 7: Commit**

```bash
git add scripts/alp_model/analyze.py tests/scripts/test_alp_model_analyze.py
git commit -m "feat(model): static fit/perf analyzer engine (analyze_model)"
```

---

## Task 4: The `alp model check` CLI

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `analyze.analyze_model` + `analyze.UnsupportedModelError` (Task 3); the `model_group` click group + `_DEFAULT_META` (existing).
- Produces: `alp model check <model> --sku <SKU> [--metadata-root DIR] [--format human|json]`.
  - `--format json` payload: `{"model", "sku", "backends": [{"backend", "verdict", "est_sram_kib", "budget_sram_kib", "est_latency_ms", "op_coverage_pct", "unsupported_ops", "source"}], "suggestion"}`.
  - Exit 0 on a completed analysis (any verdict — a `no-fit` is a valid answer, not a CLI error). Exit 1 on a real error (bad model path, unknown SKU, non-TFLite input).

- [ ] **Step 1: Write the failing CLI tests**

Add to `tests/scripts/test_alp_cli_model.py` (match the file's existing imports — `CliRunner`, `from alp_cli.main import cli`, and its `_ROOT`/fixture-path helpers):

```python
def test_model_check_json(tmp_path):
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801", "--format", "json"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert payload["sku"] == "E1M-AEN801"
    assert payload["backends"], "expected backends"
    assert all(b["source"] == "static" for b in payload["backends"])
    cpu = [b for b in payload["backends"] if b["backend"] == "cpu"]
    assert cpu and cpu[0]["verdict"] == "fits"


def test_model_check_human_lists_backends():
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    assert "ethos_u" in res.output and "cpu" in res.output
    assert "static" in res.output


def test_model_check_non_tflite_errors():
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801"])
    assert res.exit_code == 1
    assert "tflite" in res.output.lower()
```

If `test_alp_cli_model.py` has no `_ROOT`, add `_ROOT = Path(__file__).resolve().parents[2]` (with `from pathlib import Path`) near the top, matching `test_alp_model_analyze.py`.

- [ ] **Step 2: Run to verify failure**

Run: `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k check`
Expected: FAIL — no such command `check`.

- [ ] **Step 3: Implement `check_cmd`**

In `scripts/alp_cli/model.py`, add the import and the command (place `check_cmd` after `info_cmd`, before `doctor_cmd`, mirroring their decorator style):

```python
from alp_model.analyze import UnsupportedModelError, analyze_model
```

```python
@model_group.command(name="check", help="Static pre-flight fit/perf check for a model on a SoM (offline, no toolchain).")
@click.argument("model", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--sku", required=True, help="SoM SKU, e.g. E1M-AEN801.")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False,
              help="Metadata root (defaults to the SDK's metadata/).")
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def check_cmd(model: Path, sku: str, metadata_root: Path, fmt: str) -> None:
    try:
        result = analyze_model(model, sku, metadata_root=metadata_root)
    except (UnsupportedModelError, FileNotFoundError) as exc:
        raise SystemExit(f"error: {exc}")

    if fmt == "json":
        payload = {
            "model": result.model,
            "sku": result.sku,
            "backends": [
                {
                    "backend": b.backend, "verdict": b.verdict,
                    "est_sram_kib": b.est_sram_kib, "budget_sram_kib": b.budget_sram_kib,
                    "est_latency_ms": b.est_latency_ms,
                    "op_coverage_pct": b.op_coverage_pct,
                    "unsupported_ops": b.unsupported_ops, "source": b.source,
                }
                for b in result.backends
            ],
            "suggestion": result.suggestion,
        }
        click.echo(json.dumps(payload, indent=2))
        return

    click.echo(f"model: {result.model}")
    click.echo(f"SoM:   {result.sku}")
    for b in result.backends:
        lat = "n/a" if b.est_latency_ms is None else f"{b.est_latency_ms:.2f} ms"
        budget = "unknown" if b.budget_sram_kib is None else f"{b.budget_sram_kib} KiB"
        click.echo(
            f"  [{b.verdict:>12}] {b.backend:<11} "
            f"sram ~{b.est_sram_kib} KiB / budget {budget}  "
            f"latency {lat}  ops {b.op_coverage_pct:.0f}%  ({b.source})")
        if b.unsupported_ops:
            click.echo(f"               unsupported: {', '.join(b.unsupported_ops)}")
    if result.suggestion:
        click.echo(f"suggestion: {result.suggestion}")
```

Confirm `Path` and `json` are already imported at the top of `model.py` (they are — `build_cmd`/`info_cmd` use both); add them only if a build error says otherwise.

- [ ] **Step 4: Run the CLI tests**

Run: `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q`
Expected: PASS (existing model CLI tests + the 3 new `check` tests).

- [ ] **Step 5: Human smoke (verify it reads well)**

Run: `py -3.11 -m alp_cli.main model check tests/fixtures/models/tiny_int8.tflite --sku E1M-AEN801`
Expected: a `model:` / `SoM:` header, one line per backend (cpu `[        fits]`, at least one `ethos_u`), each tagged `(static)`. Sanity-check the numbers are plausible (sram ~1 KiB, ops 100%).

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model check' pre-flight fit/perf CLI"
```

---

## Task 5: Docs — the `alp model check` contract

**Files:**
- Modify: `docs/CLI.md` (or the alp-sdk model-CLI doc that Plan A/#907 updates — locate the `alp model` command reference and add `check` alongside `build`/`list`/`info`/`doctor`).

**Interfaces:** none (documentation).

- [ ] **Step 1: Locate the model-CLI doc section**

Run: `grep -rn "alp model build\|model doctor\|model list" docs/ | head`
Find the section that documents the `alp model` subcommands (added/extended by #907).

- [ ] **Step 2: Add the `check` entry**

Document, in the same style as the sibling subcommands:
- Synopsis: `alp model check <model> --sku <SKU> [--metadata-root DIR] [--format human|json]`.
- Purpose: offline static pre-flight — per-backend fit verdict (`fits`/`cpu-fallback`/`no-fit`) + conservative SRAM/latency/op-coverage estimates, `source: static`.
- The JSON payload shape (copy the `{"model","sku","backends":[…],"suggestion"}` structure from Task 4).
- The honest caveats: tier-1 estimates only (TFLite input; ONNX + tier-2 precomputed + tier-3 exact are follow-ons); SRAM `no-fit` + in-family cross-sell activate once `inference_arena_sram_kib` budgets are authored.

- [ ] **Step 3: Doc-drift gate**

Run: `py -3.11 scripts/check_doc_drift.py`
Expected: exit 0 (no dead-symbol refs / index breakage).

- [ ] **Step 4: Commit**

```bash
git add docs/
git commit -m "docs(model): document the 'alp model check' pre-flight contract"
```

---

## Self-Review

**Spec coverage** (against `docs/superpowers/specs/2026-07-24-edge-ai-lifecycle-roadmap.md` §3, §5):
- §3.1 tier-1 static estimator (offline, no toolchain) → Tasks 1+3; tiers 2/3 explicitly deferred (Global Constraints). ✓
- §3.2 verdict / est_sram_kib / est_latency_ms / op_coverage_pct / unsupported_ops / source → `BackendResult` (Task 3) + JSON payload (Task 4). ✓
- §3.3 actionable verdicts → `_suggest` (op-fallback case live; SRAM/next-bigger-SoM cross-sell deferred with reason: all arenas are 0). ✓ (partial, deliberately — see Global Constraints)
- §3.4 per-NPU op-support data asset → Task 2 (`metadata/npu_ops/*.json` + schema + validator). Per-(SoM×model) bench perf points = tier 2, deferred. ✓
- §3.5 alp-sdk surface `alp model check <model> [--sku]` → Task 4. tan-cli + extension layers = the 1b/1c follow-on plans. ✓
- §5 success #1 (offline per-backend verdict, `source: static`) → `test_analyze_model_*` + `test_model_check_json`. ✓
- §5 success #2 (`source: precomputed` for known models) → tier 2, deferred. ✗ (out of slice)
- §5 success #3 (arena overflow → next-bigger SoM) → mechanism present (`no-fit` + `_suggest`), dormant until budgets authored. ✓ (partial)
- §5 success #4 (extension fit badge + build pre-flight) → 1c follow-on plan. ✗ (out of slice)

**Deferred-to-follow-on (flagged, not gaps):** ONNX static analysis; tier-2 precomputed metadata readers; tier-3 exact-on-demand; the SoM-ordering cross-sell; `tan model check` (1b); the extension fit badge (1c). Each rides the same `source`/payload contract this slice establishes.

**Placeholder scan:** no TBD/TODO; every code step carries complete code; every test step carries real assertions with concrete expected values (8 MACs, 368640 MACs, 1 KiB, 50%). Task 2 Step 5 and Task 5 intentionally say "match the file's real symbol names" — those are the two places the exact existing identifiers must be read from the target file rather than guessed.

**Type consistency:** `OpDesc`/`TensorDesc` (Task 1) consumed unchanged in Task 3; `BackendResult`/`AnalysisResult` fields (Task 3) map 1:1 to the JSON payload keys (Task 4); `analyze_model(model_path, sku, *, metadata_root)` signature identical across Tasks 3/4; `UnsupportedModelError` raised in Task 3, caught in Task 4.
