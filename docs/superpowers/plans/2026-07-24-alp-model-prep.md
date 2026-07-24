# `alp model prep` — License-Free Quantize + Accuracy Report (Slice 3a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** `alp model prep <raw.onnx> --calibration <dir>` — quantize a model to INT8 and produce an **fp32-vs-int8 accuracy-delta report**, entirely on free `onnxruntime`/`onnx` (no vendor toolchain). Turns "quantization is a dark art" into a guided, measured flow.

**Architecture:** A pure `scripts/alp_model/prep.py` engine (validate calibration → quantize_static → run both models on the calibration set → accuracy delta) + a thin `alp model prep` CLI. License-free: uses `onnxruntime.quantization` (QDQ static) + `onnxruntime` inference only. The vendor compile (Vela/dxcom/DRP-AI) stays a separate, later step.

**Tech Stack:** Python 3.11, `onnxruntime` (>=1.17), `onnx`, `numpy`, `click`; `pytest` + `CliRunner`.

## Global Constraints

- **License-free only.** `onnxruntime.quantization.quantize_static` + `onnxruntime.InferenceSession`. NO dxcom/vela/DRP-AI imports. Any customer preps on any machine.
- **Accuracy preservation IS the deliverable.** After quantizing, run fp32 AND int8 on the calibration set and report the delta (top-1 agreement %, mean cosine similarity, max abs error) + a `verdict` (good/degraded) + concrete `guidance` when degraded (per-channel, more calibration data, keep sensitive ops fp16). A silent accuracy cliff is the failure mode this exists to prevent.
- **Calibration validation up front.** A bad calibration set is the silent accuracy killer: reject (clear error) too-few samples (`< min_samples`, default 8) or shape mismatch against the model input BEFORE quantizing.
- **ONNX input in 3a.** Raw input is `.onnx`. TFLite/PyTorch/Keras→ONNX conversion is the immediate follow-on (needs `tf2onnx`/`torch`); a non-`.onnx` input is a clear "not supported yet" error, never a wrong result.
- **Fail closed.** A quantize or inference failure raises `PrepError` with the cause — never emit a "prepped" model that didn't actually quantize or that can't run.
- **Branch:** `feat/alp-model-prep`, stacked on `feat/alp-model-zoo`. PR targets `dev`. Do NOT merge. NO Claude/AI attribution.
- **Env:** `py -3.11` (NOT py -3.14). `alp_cli` under `scripts/` (manual runs need `PYTHONPATH=scripts`). `onnxruntime`+`onnx` installed. Gate before Python commits: `py -3.11 -m pytest tests/scripts/test_alp_model_prep.py tests/scripts/test_alp_cli_model.py -q`.

---

## File Structure

- **Create** `scripts/alp_model/prep.py` — the engine: `PrepError`, `CalibrationInfo`, `AccuracyReport`, `model_input`, `load_calibration`, `validate_calibration`, `_NpyReader`, `quantize`, `accuracy_delta`.
- **Modify** `scripts/alp_cli/model.py` — `prep_cmd`.
- **Modify** `pyproject.toml` — add a `model-prep` optional-dependencies extra (`onnxruntime`, `onnx`, `numpy`).
- **Create** `tests/scripts/test_alp_model_prep.py` — engine tests (against `tests/fixtures/models/tiny_cnn.onnx`, gated `importorskip("onnxruntime")`).
- **Modify** `tests/scripts/test_alp_cli_model.py` — `model prep` CLI test.

---

## Task 1: Calibration loading + validation

**Files:**
- Create: `scripts/alp_model/prep.py`
- Test: `tests/scripts/test_alp_model_prep.py` (new)

**Interfaces:**
- Produces:
  - `class PrepError(Exception)`.
  - `@dataclass(frozen=True) class CalibrationInfo { samples: int; input_name: str; input_shape: list[int] }`.
  - `model_input(onnx_path: Path) -> tuple[str, list[int]]` — the first input's (name, shape); a `None`/dynamic dim → treated as batch (recorded as -1).
  - `load_calibration(cal_dir: Path, input_shape: list[int]) -> list[np.ndarray]` — load every `*.npy`, each reshaped/validated to a single-sample batch matching `input_shape` (dynamic/-1 dims wildcard); raise `PrepError` on a shape that can't match.
  - `validate_calibration(cal_dir: Path, onnx_path: Path, *, min_samples: int = 8) -> CalibrationInfo` — raise `PrepError` if `< min_samples` or any sample mismatches.

- [ ] **Step 1: Write the failing tests**

Create `tests/scripts/test_alp_model_prep.py`:

```python
"""alp_model.prep — license-free quantize + accuracy report."""
import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))
_ONNX = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


def _make_calib(dirpath: Path, n: int, shape=(1, 3, 224, 224)):
    dirpath.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(0)
    for i in range(n):
        np.save(dirpath / f"s{i}.npy", rng.standard_normal(shape).astype(np.float32))


def test_model_input_shape():
    pytest.importorskip("onnxruntime")
    from alp_model.prep import model_input
    name, shape = model_input(_ONNX)
    assert name == "input"
    assert shape[-2:] == [224, 224]


def test_validate_calibration_ok_and_too_few(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import validate_calibration, PrepError
    _make_calib(tmp_path / "cal", 8)
    info = validate_calibration(tmp_path / "cal", _ONNX, min_samples=8)
    assert info.samples == 8 and info.input_name == "input"
    _make_calib(tmp_path / "cal2", 2)
    with pytest.raises(PrepError):
        validate_calibration(tmp_path / "cal2", _ONNX, min_samples=8)


def test_load_calibration_shape_mismatch_raises(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import load_calibration, PrepError
    d = tmp_path / "bad"; d.mkdir()
    np.save(d / "wrong.npy", np.zeros((1, 3, 64, 64), dtype=np.float32))
    with pytest.raises(PrepError):
        load_calibration(d, [1, 3, 224, 224])
```

- [ ] **Step 2: Run → fail** (`ModuleNotFoundError: alp_model.prep`).

- [ ] **Step 3: Implement the calibration half of `prep.py`**

Create `scripts/alp_model/prep.py`:

```python
# scripts/alp_model/prep.py
"""License-free model prep: validate calibration, INT8-quantize (onnxruntime QDQ
static), and report the fp32-vs-int8 accuracy delta. No vendor toolchain — the
accuracy report is the value (a silent INT8 accuracy cliff is the failure mode
this prevents). Vendor compile (Vela/dxcom/DRP-AI) is a separate later step."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np


class PrepError(Exception):
    """Calibration invalid, or a quantize/inference step failed — raised instead
    of emitting a model that didn't actually quantize or can't run."""


@dataclass(frozen=True)
class CalibrationInfo:
    samples: int
    input_name: str
    input_shape: list[int]


def model_input(onnx_path: Path) -> tuple[str, list[int]]:
    import onnxruntime as ort
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0]
    shape = [(-1 if not isinstance(d, int) else d) for d in inp.shape]
    return inp.name, shape


def _shape_matches(sample: np.ndarray, want: list[int]) -> bool:
    s = list(sample.shape)
    if len(s) != len(want):
        # allow a sample missing the leading batch dim
        if len(s) == len(want) - 1 and want[0] in (-1, 1):
            s = [1, *s]
        else:
            return False
    return all(w in (-1, d) for d, w in zip(s, want))


def load_calibration(cal_dir: Path, input_shape: list[int]) -> list[np.ndarray]:
    files = sorted(Path(cal_dir).glob("*.npy"))
    if not files:
        raise PrepError(f"no .npy calibration samples in {cal_dir}")
    out: list[np.ndarray] = []
    for f in files:
        arr = np.load(f).astype(np.float32)
        if arr.ndim == len(input_shape) - 1:
            arr = arr[None, ...]                       # add batch dim
        if not _shape_matches(arr, input_shape):
            raise PrepError(
                f"calibration sample {f.name} shape {list(arr.shape)} "
                f"!= model input {input_shape}")
        out.append(arr)
    return out


def validate_calibration(cal_dir: Path, onnx_path: Path, *,
                         min_samples: int = 8) -> CalibrationInfo:
    name, shape = model_input(onnx_path)
    samples = load_calibration(cal_dir, shape)
    if len(samples) < min_samples:
        raise PrepError(
            f"calibration set has {len(samples)} samples; need >= {min_samples} "
            f"for a representative INT8 range")
    return CalibrationInfo(samples=len(samples), input_name=name, input_shape=shape)
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_model_prep.py -q`).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/prep.py tests/scripts/test_alp_model_prep.py
git commit -m "feat(model): prep — calibration loading + validation"
```

---

## Task 2: Quantize + accuracy delta

**Files:**
- Modify: `scripts/alp_model/prep.py`
- Test: `tests/scripts/test_alp_model_prep.py` (extend)

**Interfaces:**
- Consumes: Task 1's `model_input`/`load_calibration`/`validate_calibration`/`PrepError`.
- Produces:
  - `class _NpyReader(CalibrationDataReader)` — feeds `{input_name: sample}` until exhausted.
  - `quantize(raw_onnx: Path, out_onnx: Path, cal_dir: Path, *, per_channel: bool = False) -> Path` — pre-process (shape inference) then `quantize_static` (QDQ, QInt8 weights). Returns `out_onnx`.
  - `@dataclass(frozen=True) class AccuracyReport { samples: int; top1_agreement_pct: float; mean_cosine: float; max_abs_err: float; verdict: str; guidance: str | None }`.
  - `accuracy_delta(fp32_onnx: Path, int8_onnx: Path, cal_dir: Path, *, degrade_threshold: float = 95.0) -> AccuracyReport`.

- [ ] **Step 1: Write the failing tests**

Extend `tests/scripts/test_alp_model_prep.py`:

```python
def test_quantize_and_accuracy_report(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import quantize, accuracy_delta
    cal = tmp_path / "cal"; _make_calib(cal, 8)
    out = tmp_path / "tiny.int8.onnx"
    q = quantize(_ONNX, out, cal)
    assert q.is_file() and q.stat().st_size > 0
    # the quantized model must actually load + run
    import onnxruntime as ort
    ort.InferenceSession(str(q), providers=["CPUExecutionProvider"])
    rep = accuracy_delta(_ONNX, q, cal)
    assert rep.samples == 8
    assert 0.0 <= rep.top1_agreement_pct <= 100.0
    assert -1.0 <= rep.mean_cosine <= 1.0001
    assert rep.verdict in ("good", "degraded")
    assert (rep.guidance is None) == (rep.verdict == "good")
```

- [ ] **Step 2: Run → fail** (`cannot import name 'quantize'`).

- [ ] **Step 3: Implement the quantize + accuracy half of `prep.py`**

Append to `scripts/alp_model/prep.py`:

```python
def _import_quant():
    from onnxruntime.quantization import (CalibrationDataReader, QuantFormat,
                                          QuantType, quantize_static)
    from onnxruntime.quantization.shape_inference import quant_pre_process
    return CalibrationDataReader, QuantFormat, QuantType, quantize_static, quant_pre_process


def _reader_cls(input_name: str, samples: list[np.ndarray]):
    CalibrationDataReader = _import_quant()[0]

    class _NpyReader(CalibrationDataReader):
        def __init__(self) -> None:
            self._it = iter([{input_name: s} for s in samples])

        def get_next(self):
            return next(self._it, None)

    return _NpyReader()


def quantize(raw_onnx: Path, out_onnx: Path, cal_dir: Path, *,
             per_channel: bool = False) -> Path:
    if Path(raw_onnx).suffix.lower() != ".onnx":
        raise PrepError(
            f"model prep supports .onnx input in this release; got {Path(raw_onnx).name} "
            f"(TFLite/PyTorch/Keras -> ONNX conversion is a follow-on)")
    _, _, QuantType, quantize_static, quant_pre_process = _import_quant()
    name, shape = model_input(raw_onnx)
    samples = load_calibration(cal_dir, shape)
    out_onnx = Path(out_onnx)
    out_onnx.parent.mkdir(parents=True, exist_ok=True)
    pre = out_onnx.with_suffix(".pre.onnx")
    try:
        quant_pre_process(str(raw_onnx), str(pre))
        quantize_static(str(pre), str(out_onnx), _reader_cls(name, samples),
                        per_channel=per_channel, weight_type=QuantType.QInt8)
    except Exception as exc:                      # onnxruntime raises broad types
        raise PrepError(f"quantization failed: {exc}") from exc
    finally:
        if pre.is_file():
            pre.unlink()
    if not out_onnx.is_file():
        raise PrepError("quantization produced no output model")
    return out_onnx


@dataclass(frozen=True)
class AccuracyReport:
    samples: int
    top1_agreement_pct: float
    mean_cosine: float
    max_abs_err: float
    verdict: str
    guidance: str | None


def _cosine(a: np.ndarray, b: np.ndarray) -> float:
    na, nb = float(np.linalg.norm(a)), float(np.linalg.norm(b))
    if na == 0.0 or nb == 0.0:
        return 1.0 if na == nb else 0.0
    return float(np.dot(a, b) / (na * nb))


def accuracy_delta(fp32_onnx: Path, int8_onnx: Path, cal_dir: Path, *,
                   degrade_threshold: float = 95.0) -> AccuracyReport:
    import onnxruntime as ort
    name, shape = model_input(fp32_onnx)
    samples = load_calibration(cal_dir, shape)
    s32 = ort.InferenceSession(str(fp32_onnx), providers=["CPUExecutionProvider"])
    s8 = ort.InferenceSession(str(int8_onnx), providers=["CPUExecutionProvider"])
    agree, coss, maxerr = 0, [], 0.0
    for x in samples:
        o32 = s32.run(None, {name: x})[0].ravel()
        o8 = s8.run(None, {name: x})[0].ravel()
        if int(o32.argmax()) == int(o8.argmax()):
            agree += 1
        coss.append(_cosine(o32, o8))
        maxerr = max(maxerr, float(np.abs(o32 - o8).max()))
    top1 = agree / len(samples) * 100.0
    verdict = "good" if top1 >= degrade_threshold else "degraded"
    guidance = None if verdict == "good" else (
        "INT8 accuracy dropped: try --per-channel, add more representative "
        "calibration samples, or keep sensitive ops in fp16.")
    return AccuracyReport(samples=len(samples), top1_agreement_pct=round(top1, 1),
                          mean_cosine=round(float(np.mean(coss)), 4),
                          max_abs_err=round(maxerr, 6), verdict=verdict,
                          guidance=guidance)
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_model_prep.py -q`). If `quantize_static` errors on the fixture, iterate on the pre-process/quant options until the quantized model loads + runs — the test asserts exactly that.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/prep.py tests/scripts/test_alp_model_prep.py
git commit -m "feat(model): prep — INT8 quantize + fp32-vs-int8 accuracy delta"
```

---

## Task 3: `alp model prep` CLI

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `validate_calibration`, `quantize`, `accuracy_delta`, `PrepError` (Tasks 1-2).
- Produces: `alp model prep <raw.onnx> --calibration <dir> [--out FILE] [--per-channel] [--min-samples N] [--format human|json]`. JSON: `{"raw","quantized","accuracy":{samples,top1_agreement_pct,mean_cosine,max_abs_err,verdict,guidance}}`. Exit 1 on `PrepError` (bad calibration / non-onnx / quantize failure).

- [ ] **Step 1: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py`:

```python
def test_model_prep_json(tmp_path):
    import json
    import numpy as np
    import pytest as _pytest
    _pytest.importorskip("onnxruntime")
    from click.testing import CliRunner
    from alp_cli.main import cli
    cal = tmp_path / "cal"; cal.mkdir()
    rng = np.random.default_rng(0)
    for i in range(8):
        np.save(cal / f"s{i}.npy", rng.standard_normal((1, 3, 224, 224)).astype(np.float32))
    raw = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    out = tmp_path / "tiny.int8.onnx"
    res = CliRunner().invoke(cli, ["model", "prep", str(raw), "--calibration", str(cal),
                                   "--out", str(out), "--format", "json"],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert out.is_file()
    assert payload["accuracy"]["samples"] == 8
    assert payload["accuracy"]["verdict"] in ("good", "degraded")


def test_model_prep_too_few_calibration_errors(tmp_path):
    import numpy as np
    import pytest as _pytest
    _pytest.importorskip("onnxruntime")
    from click.testing import CliRunner
    from alp_cli.main import cli
    cal = tmp_path / "cal"; cal.mkdir()
    np.save(cal / "s0.npy", np.zeros((1, 3, 224, 224), dtype=np.float32))
    raw = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    res = CliRunner().invoke(cli, ["model", "prep", str(raw), "--calibration", str(cal)])
    assert res.exit_code == 1
```

- [ ] **Step 2: Run → fail** (`no such command 'prep'`).

- [ ] **Step 3: Implement `prep_cmd`** in `scripts/alp_cli/model.py` (import + command after `add_cmd`):

```python
from alp_model.prep import PrepError, accuracy_delta, quantize, validate_calibration
```

```python
@model_group.command(name="prep", help="License-free INT8 quantize + fp32-vs-int8 accuracy report.")
@click.argument("raw", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--calibration", "cal_dir", required=True,
              type=click.Path(exists=True, file_okay=False, path_type=Path),
              help="Directory of .npy calibration samples matching the model input.")
@click.option("--out", default=None, type=click.Path(path_type=Path),
              help="Output INT8 .onnx (default: <raw>.int8.onnx).")
@click.option("--per-channel", is_flag=True, help="Per-channel weight quantization (often recovers accuracy).")
@click.option("--min-samples", default=8, show_default=True, help="Minimum calibration samples.")
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def prep_cmd(raw: Path, cal_dir: Path, out: Path | None, per_channel: bool,
             min_samples: int, fmt: str) -> None:
    out = out or raw.with_suffix(".int8.onnx")
    try:
        validate_calibration(cal_dir, raw, min_samples=min_samples)
        quantize(raw, out, cal_dir, per_channel=per_channel)
        rep = accuracy_delta(raw, out, cal_dir)
    except PrepError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    acc = {"samples": rep.samples, "top1_agreement_pct": rep.top1_agreement_pct,
           "mean_cosine": rep.mean_cosine, "max_abs_err": rep.max_abs_err,
           "verdict": rep.verdict, "guidance": rep.guidance}
    if fmt == "json":
        click.echo(json.dumps({"raw": str(raw), "quantized": str(out), "accuracy": acc}, indent=2))
        return
    click.echo(f"quantized: {out}")
    click.echo(f"accuracy: {rep.verdict}  top1={rep.top1_agreement_pct}%  "
               f"cosine={rep.mean_cosine}  max_abs_err={rep.max_abs_err}  (n={rep.samples})")
    if rep.guidance:
        click.echo(f"guidance: {rep.guidance}")
```

- [ ] **Step 4: Run tests green** + human smoke:

```bash
py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k prep
```

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model prep' (license-free quantize + accuracy report)"
```

---

## Task 4: Declare the `model-prep` optional dependency + docs

**Files:**
- Modify: `pyproject.toml`
- Modify: `docs/cli.md` (the `model` command reference)

**Interfaces:** none.

- [ ] **Step 1: Add the extra**

In `pyproject.toml` `[project.optional-dependencies]`, add (mirror the existing `model-io`/`model-compile` extras' style):

```toml
model-prep = ["onnxruntime>=1.17", "onnx>=1.15", "numpy>=1.23"]
```

- [ ] **Step 2: Document `model prep`** in `docs/cli.md` (same style as the sibling `model` subcommands): synopsis `alp/tan model prep <raw.onnx> --calibration <dir> [--out] [--per-channel]`; that it's license-free (onnxruntime only); the JSON payload (`{raw,quantized,accuracy:{...}}`); the accuracy verdict/guidance semantics; the ONNX-only + vendor-compile-separate caveats.

- [ ] **Step 3: Gates**

```bash
py -3.11 -m pytest tests/scripts/test_alp_model_prep.py tests/scripts/test_alp_cli_model.py -q
py -3.11 scripts/check_doc_drift.py
```

- [ ] **Step 4: Commit**

```bash
git add pyproject.toml docs/cli.md
git commit -m "chore(model): declare model-prep extra + document alp model prep"
```

---

## Self-Review

- **Spec coverage** (roadmap §4 sub-project 3): license-free convert/quantize/accuracy-check on onnxruntime/onnx ✓ (quantize is license-free; TFLite/PyTorch→ONNX conversion = follow-on); accuracy-preservation via fp32-vs-int8 delta report ✓ (Task 2); calibration-set validation (min N + shape) ✓ (Task 1); `alp model prep <raw> --calibration` surface ✓ (Task 3). Per-backend compile-config defaults + `board.yaml compile:` wiring + the extension wizard = follow-ons.
- **Placeholder scan:** all code complete; onnxruntime quant call may need iteration (Task 2 Step 4 flags this explicitly — the test pins the required outcome: quantized model loads + runs).
- **Type consistency:** `CalibrationInfo`/`AccuracyReport` fields identical across engine, CLI, and tests; `quantize(raw, out, cal_dir, *, per_channel)` + `accuracy_delta(fp32, int8, cal_dir)` signatures identical in engine + `prep_cmd`; `PrepError` raised in engine, caught in CLI.
- **Honesty:** the accuracy report is real (both models actually run on the calibration set); on random test calibration data the numbers are meaningless but the machinery is exercised — real accuracy validation shows on the user's real model + data, exactly where the value is.
