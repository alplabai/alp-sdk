# `alp model prep` — TFLite→ONNX Conversion (Slice 3d) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Let `alp model prep` accept a `.tflite` model (the SDK's native format) — convert it to ONNX via `tf2onnx`, then run the existing quantize + accuracy flow.

**Architecture:** A new pure `scripts/alp_model/convert.py` (`to_onnx` — `.onnx` passthrough, `.tflite`→`tf2onnx.convert.from_tflite`) + `prep_cmd` converts a `.tflite` raw to a temp ONNX before quantizing. The conversion deps (`tf2onnx` + `tensorflow`) are a NEW optional extra (heavy — kept out of the base + the lighter `model-prep` extra).

**Tech Stack:** Python 3.11, `tf2onnx` (1.17), `tensorflow-cpu` (2.21, pulled by the tflite path at runtime), `onnx`, `onnxruntime`, `click`.

## Global Constraints

- **Additive + backward-compatible.** `.onnx` prep is byte-identical to before; `.tflite` is the new accepted input. No other prep behaviour changes.
- **Fail closed + clear deps message.** A `.tflite` input with `tf2onnx`/`tensorflow` missing → a clear `ConvertError` naming the extra (`pip install alp-sdk-cli[model-convert]`), never a raw `ModuleNotFoundError`. A conversion failure → `ConvertError`, never a partial/garbage ONNX fed to quantize.
- **Conversion needs `tf2onnx` + `tensorflow` (heavy).** Declare a NEW `model-convert` extra; do NOT add tensorflow to the base or the `model-prep` extra. The `.tflite` tests gate on `pytest.importorskip("tf2onnx")` + `importorskip("tensorflow")`.
- **`to_onnx` is pure-ish** — lazy-imports `tf2onnx` only inside the `.tflite` branch (so importing `convert.py` doesn't drag in tensorflow).
- **Branch:** `feat/alp-model-tflite-convert`, stacked on `feat/alp-model-measure` (the edge-AI stack tip). Do NOT merge. NO Claude/AI attribution. Public-repo hygiene — no local absolute paths in committed files.
- **Env:** `py -3.11` (NOT py -3.14). `tf2onnx` 1.17 + `tensorflow-cpu` 2.21 installed. `alp_cli` under `scripts/`. Gate before Python commits: `py -3.11 -m pytest tests/scripts/test_alp_model_convert.py tests/scripts/test_alp_cli_model.py -q`.

---

## File Structure

- **Create** `scripts/alp_model/convert.py` — `ConvertError`, `to_onnx(src, dst, *, opset=13)`.
- **Modify** `scripts/alp_cli/model.py` — `prep_cmd` accepts `.tflite` (converts first).
- **Modify** `pyproject.toml` — new `model-convert` extra (`tf2onnx`, `tensorflow-cpu`).
- **Create** `tests/fixtures/models/tiny_fp32.tflite` — a tiny fp32 TFLite model (generated once, committed).
- **Create** `tests/scripts/test_alp_model_convert.py`.
- **Modify** `tests/scripts/test_alp_cli_model.py` — `model prep <tflite>` test.
- **Modify** `docs/cli.md` — document the `.tflite` input + `model-convert` extra.

---

## Task 1: The converter (`convert.py`)

**Files:**
- Create: `scripts/alp_model/convert.py`
- Test: `tests/scripts/test_alp_model_convert.py` (new)

**Interfaces:**
- Produces:
  - `class ConvertError(Exception)`.
  - `to_onnx(src: Path, dst: Path, *, opset: int = 13) -> Path` — `.onnx` → returns `src` unchanged (no write); `.tflite` → converts via `tf2onnx.convert.from_tflite` + `onnx.save(dst)`, returns `dst`; other suffix → `ConvertError`. Missing `tf2onnx`/`tensorflow` → `ConvertError` naming the `model-convert` extra. A conversion exception → `ConvertError` (no partial file left).

- [ ] **Step 1: Write the failing tests**

Create `tests/scripts/test_alp_model_convert.py`:

```python
"""alp_model.convert — TFLite -> ONNX."""
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))
_TFLITE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
_ONNX = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


def test_onnx_passthrough(tmp_path):
    from alp_model.convert import to_onnx
    out = to_onnx(_ONNX, tmp_path / "x.onnx")
    assert out == _ONNX                       # unchanged, no conversion


def test_bad_suffix_raises(tmp_path):
    from alp_model.convert import to_onnx, ConvertError
    src = tmp_path / "m.pt"; src.write_bytes(b"x")
    with pytest.raises(ConvertError):
        to_onnx(src, tmp_path / "o.onnx")


def test_tflite_to_onnx_converts_and_loads(tmp_path):
    pytest.importorskip("tf2onnx")
    pytest.importorskip("tensorflow")
    import onnxruntime as ort
    from alp_model.convert import to_onnx
    out = to_onnx(_TFLITE, tmp_path / "conv.onnx")
    assert out.is_file() and out.stat().st_size > 0
    ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])  # must load
```

- [ ] **Step 2: Run → fail** (`ModuleNotFoundError: alp_model.convert`).

- [ ] **Step 3: Implement `convert.py`**

Create `scripts/alp_model/convert.py`:

```python
# scripts/alp_model/convert.py
"""Convert a source model to ONNX for the prep pipeline. `.onnx` passes through;
`.tflite` (the SDK's native format) converts via tf2onnx. The conversion deps
(tf2onnx + tensorflow) are the heavy `model-convert` extra, lazy-imported only
for the .tflite path so importing this module stays cheap."""
from __future__ import annotations

from pathlib import Path


class ConvertError(Exception):
    """A source model could not be converted to ONNX (unsupported format,
    missing conversion deps, or a tf2onnx failure)."""


def to_onnx(src: Path, dst: Path, *, opset: int = 13) -> Path:
    suffix = src.suffix.lower()
    if suffix == ".onnx":
        return src                                  # already ONNX — no conversion
    if suffix != ".tflite":
        raise ConvertError(
            f"unsupported model format {src.suffix}; expected .onnx or .tflite")
    try:
        import onnx
        import tf2onnx
    except ImportError as exc:
        raise ConvertError(
            ".tflite input needs the conversion toolchain — "
            "pip install alp-sdk-cli[model-convert]") from exc
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        model_proto, _ = tf2onnx.convert.from_tflite(str(src), opset=opset)
        onnx.save(model_proto, str(dst))
    except Exception as exc:                         # tf2onnx raises broad types
        if dst.is_file():
            dst.unlink()                             # no partial/garbage ONNX
        raise ConvertError(
            f"TFLite->ONNX conversion failed for {src.name}: {exc}") from exc
    return dst
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_model_convert.py -q`).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/convert.py tests/scripts/test_alp_model_convert.py
git commit -m "feat(model): TFLite->ONNX converter (to_onnx) for the prep pipeline"
```

---

## Task 2: fp32 TFLite fixture + `prep` accepts `.tflite` + the extra

**Files:**
- Create: `tests/fixtures/models/tiny_fp32.tflite`
- Modify: `scripts/alp_cli/model.py` (`prep_cmd`)
- Modify: `pyproject.toml`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `to_onnx`, `ConvertError` (Task 1); the existing `validate_calibration`/`quantize`/`accuracy_delta`/`PrepError`.

- [ ] **Step 1: Generate + commit the fp32 TFLite fixture**

Run this once (it needs tensorflow, which is installed) to create a tiny fp32 model, then commit the binary:

```bash
py -3.11 - <<'PY'
import tensorflow as tf
m = tf.keras.Sequential([tf.keras.layers.Input((4,)), tf.keras.layers.Dense(2)])
conv = tf.lite.TFLiteConverter.from_keras_model(m)   # fp32 (no quantization)
open("tests/fixtures/models/tiny_fp32.tflite", "wb").write(conv.convert())
print("wrote tiny_fp32.tflite")
PY
```

Confirm it loads via `to_onnx` + onnxruntime (input shape is `[1,4]` or `[-1,4]`).

- [ ] **Step 2: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py`:

```python
def test_model_prep_accepts_tflite(tmp_path):
    import json
    import numpy as np
    import pytest as _pytest
    _pytest.importorskip("tf2onnx")
    _pytest.importorskip("tensorflow")
    from click.testing import CliRunner
    from alp_cli.main import cli
    cal = tmp_path / "cal"; cal.mkdir()
    rng = np.random.default_rng(0)
    for i in range(8):
        np.save(cal / f"s{i}.npy", rng.standard_normal((1, 4)).astype(np.float32))
    raw = _ROOT / "tests/fixtures/models/tiny_fp32.tflite"
    out = tmp_path / "tiny.int8.onnx"
    res = CliRunner().invoke(cli, ["model", "prep", str(raw), "--calibration", str(cal),
                                   "--out", str(out), "--format", "json"],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert out.is_file()                              # quantized ONNX produced
    assert payload["accuracy"]["verdict"] in ("good", "degraded")
```

(If `tiny_fp32.tflite`'s converted input shape differs from `[1,4]`, adjust the calibration sample shape to match — read it with `to_onnx` + `model_input` first.)

- [ ] **Step 3: Extend `prep_cmd`** in `scripts/alp_cli/model.py`

Add the import:

```python
from alp_model.convert import ConvertError, to_onnx
```

Replace the `.onnx`-only guard + convert a `.tflite` before quantizing. The current body starts:
```python
    if raw.suffix.lower() != ".onnx":
        click.echo(f"error: model prep supports .onnx input in this release; got {raw.name}", err=True)
        raise SystemExit(1)
    out = out or raw.with_suffix(".int8.onnx")
```
Change to:
```python
    if raw.suffix.lower() not in (".onnx", ".tflite"):
        click.echo(f"error: model prep supports .onnx/.tflite input; got {raw.name}", err=True)
        raise SystemExit(1)
    out = out or raw.with_suffix(".int8.onnx")
    onnx_in = raw
    converted: Path | None = None
    if raw.suffix.lower() == ".tflite":
        converted = out.with_name(raw.stem + ".converted.onnx")
        try:
            onnx_in = to_onnx(raw, converted)
        except ConvertError as exc:
            click.echo(f"error: {exc}", err=True)
            raise SystemExit(1)
```
Then change the three engine calls to use `onnx_in` instead of `raw`:
```python
    try:
        validate_calibration(cal_dir, onnx_in, min_samples=min_samples)
        quantize(onnx_in, out, cal_dir, per_channel=per_channel)
        rep = accuracy_delta(onnx_in, out, cal_dir)
    except PrepError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    finally:
        if converted is not None and converted.is_file():
            converted.unlink()                        # drop the intermediate fp32 ONNX
```
(Keep the rest — the `{raw: str(raw), quantized: str(out), accuracy}` payload — unchanged; `raw` stays the original `.tflite` path.)

- [ ] **Step 4: Add the `model-convert` extra** in `pyproject.toml` `[project.optional-dependencies]` (mirror the `model-prep` extra's style):

```toml
model-convert = ["tf2onnx>=1.16", "tensorflow-cpu>=2.15"]
```

- [ ] **Step 5: Run tests green**

```bash
py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k prep
```
(the new `.tflite` test + the existing `.onnx` prep tests all pass).

- [ ] **Step 6: Commit**

```bash
git add tests/fixtures/models/tiny_fp32.tflite scripts/alp_cli/model.py pyproject.toml tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): 'alp model prep' accepts .tflite (tf2onnx convert) + model-convert extra"
```

---

## Task 3: Docs

**Files:** Modify `docs/cli.md`.

- [ ] **Step 1: Document** in the `model prep` section: `.tflite` is now accepted (converted to ONNX via `tf2onnx` before quantizing); it needs the `model-convert` extra (`pip install alp-sdk-cli[model-convert]`, which adds `tf2onnx` + `tensorflow`); `.onnx` still works with just `model-prep`. Note PyTorch/Keras→ONNX is a further follow-on.

- [ ] **Step 2: Doc-drift** (`py -3.11 scripts/check_doc_drift.py` → OK).

- [ ] **Step 3: Commit** (`git add docs/ && git commit -m "docs(model): document .tflite prep input + model-convert extra"`).

---

## Self-Review

- **Spec coverage** (roadmap §4 sub-project 3: "convert TFLite/PyTorch/Keras → ONNX"): TFLite→ONNX ✓ (the SDK's native format); PyTorch/Keras→ONNX = further follow-on (torch/keras deps). ✓
- **Placeholder scan:** all code complete; Task 2 Step 2 flags the one judgment call (match the calibration shape to the converted fixture's input).
- **Fail-closed:** missing deps → clear `ConvertError` naming the extra; conversion failure → `ConvertError` + no partial ONNX; the intermediate is cleaned in `finally`.
- **Backward-compat:** `.onnx` prep path unchanged (`to_onnx` returns src as-is); only the guard widened + a convert step added for `.tflite`.
- **Type consistency:** `to_onnx(src, dst, *, opset)` + `ConvertError` identical in `convert.py` + `prep_cmd`.
