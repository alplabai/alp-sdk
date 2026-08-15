# `alp model run` / `alp model ab` — Measure Scaffold (Slice 4a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** `alp model run <model.onnx>` (a host reference run: functional + accuracy + host-latency) and `alp model ab <a> <b> --input` (side-by-side compare) — the measurement result schema + A/B engine + estimate-vs-measured calibration feedback, all on the host with no silicon.

**Architecture:** A pure `scripts/alp_model/measure.py` (host inference via `onnxruntime` CPU, a `RunResult` telemetry record, an A/B `compare`, and `estimate_vs_measured` for the self-improving loop) + two thin CLI commands. The **on-device** run (real target latency + per-rail power via the flashed runner + on-board monitor IC) is the HW-gated follow-on that fills the SAME `RunResult` schema with real numbers.

**Tech Stack:** Python 3.11, `onnxruntime`, `numpy`, `click`; `pytest`.

## Global Constraints

- **Honesty about what's measured.** The host run is labelled `backend: "cpu-host"` — a functional + host-latency REFERENCE, explicitly NOT the target SoM's latency. On-device measurement (Ethos-U/DRP-AI/DEEPX target latency, peak SRAM, energy-per-inference) is the HW-gated follow-on. Never present a host number as a target number.
- **Power + peak-SRAM are HW-only fields.** `RunResult.power_mj` and `RunResult.peak_sram_kib` are `None` on the host — they're populated ONLY by an on-device run reading the on-board shunt/monitor IC (pending the per-SoM power-topology metadata) / the target's arena high-water. Carry the fields (the schema is stable across host + device); leave them `None` here.
- **The self-improving loop is real.** `estimate_vs_measured` compares #1's static `est_latency_ms` (`alp model check`) against a measured `latency_ms` → a ratio that (later) calibrates the tier-1 estimator. Decoupled: it takes two numbers, not the check payload.
- **A/B is deterministic.** `compare` is pure given two `RunResult`s (+ optional file sizes). No hidden timing inside `compare` — timing happens only in `run_host`.
- **Branch:** `feat/alp-model-measure`, stacked on `feat/alp-model-prep`. PR targets `dev`. Do NOT merge. NO Claude/AI attribution.
- **Env:** `py -3.11` (NOT py -3.14). `onnxruntime`/`onnx`/`numpy` installed. `alp_cli` under `scripts/`. Gate before Python commits: `py -3.11 -m pytest tests/scripts/test_alp_model_measure.py tests/scripts/test_alp_cli_model.py -q`.

---

## File Structure

- **Create** `scripts/alp_model/measure.py` — `MeasureError`, `RunResult`, `run_host`, `accuracy_vs`, `ABComparison`, `compare`, `estimate_vs_measured`.
- **Modify** `scripts/alp_cli/model.py` — `run_cmd` + `ab_cmd`.
- **Create** `tests/scripts/test_alp_model_measure.py`.
- **Modify** `tests/scripts/test_alp_cli_model.py` — `model run` + `model ab` CLI tests.
- **Modify** `docs/cli.md` — document `model run` / `model ab` + the host-vs-device honesty.

---

## Task 1: The measure engine (`measure.py`)

**Files:**
- Create: `scripts/alp_model/measure.py`
- Test: `tests/scripts/test_alp_model_measure.py` (new)

**Interfaces:**
- Produces:
  - `class MeasureError(Exception)`.
  - `@dataclass(frozen=True) class RunResult { backend: str; latency_ms: float; output_argmax: int | None; peak_sram_kib: int | None; power_mj: float | None; runs: int }`.
  - `run_host(onnx_path: Path, input_array: np.ndarray, *, runs: int = 20) -> RunResult` — onnxruntime CPU session; warm up once, time `runs` inferences, `latency_ms` = median (ms). `backend="cpu-host"`, `peak_sram_kib=None`, `power_mj=None`. Raise `MeasureError` if the model won't load/run.
  - `accuracy_vs(output_argmax: int | None, expected_label: int) -> bool`.
  - `@dataclass(frozen=True) class ABComparison { faster: str; latency_ratio: float; a_latency_ms: float; b_latency_ms: float; size_delta_bytes: int | None }`.
  - `compare(a: RunResult, b: RunResult, *, size_a: int | None = None, size_b: int | None = None) -> ABComparison` — `faster` in {"a","b","tie"}; `latency_ratio = b.latency_ms / a.latency_ms`.
  - `estimate_vs_measured(est_latency_ms: float | None, measured_latency_ms: float) -> dict` — `{"est_latency_ms", "measured_latency_ms", "ratio"}` (`ratio = measured/est` or `None` if est is falsy).

- [ ] **Step 1: Write the failing tests**

Create `tests/scripts/test_alp_model_measure.py`:

```python
"""alp_model.measure — host reference run + A/B + calibration feedback."""
import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))
_ONNX = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


def test_run_host_produces_timed_result():
    pytest.importorskip("onnxruntime")
    from alp_model.measure import run_host
    x = np.random.default_rng(0).standard_normal((1, 3, 224, 224)).astype(np.float32)
    r = run_host(_ONNX, x, runs=5)
    assert r.backend == "cpu-host"
    assert r.latency_ms > 0.0 and r.runs == 5
    assert isinstance(r.output_argmax, int)
    assert r.power_mj is None and r.peak_sram_kib is None   # HW-only fields


def test_compare_picks_faster_and_ratio():
    from alp_model.measure import RunResult, compare
    a = RunResult("cpu-host", 10.0, 3, None, None, 5)
    b = RunResult("cpu-host", 20.0, 3, None, None, 5)
    c = compare(a, b, size_a=100, size_b=250)
    assert c.faster == "a" and c.latency_ratio == 2.0
    assert c.size_delta_bytes == 150


def test_estimate_vs_measured_ratio():
    from alp_model.measure import estimate_vs_measured
    d = estimate_vs_measured(2.0, 5.0)
    assert d["ratio"] == 2.5
    assert estimate_vs_measured(None, 5.0)["ratio"] is None


def test_accuracy_vs():
    from alp_model.measure import accuracy_vs
    assert accuracy_vs(3, 3) is True
    assert accuracy_vs(3, 4) is False
    assert accuracy_vs(None, 4) is False
```

- [ ] **Step 2: Run → fail** (`ModuleNotFoundError: alp_model.measure`).

- [ ] **Step 3: Implement `measure.py`**

Create `scripts/alp_model/measure.py`:

```python
# scripts/alp_model/measure.py
"""Measure: a host reference run + A/B compare + estimate-vs-measured feedback.

The host run (`backend="cpu-host"`) is a FUNCTIONAL + host-latency reference,
NOT the target SoM's performance. On-device measurement (target latency, peak
arena SRAM, per-rail energy via the on-board monitor IC) is the HW-gated
follow-on that fills the SAME RunResult schema with real numbers. `power_mj`
and `peak_sram_kib` stay None on the host."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from statistics import median
from time import perf_counter

import numpy as np


class MeasureError(Exception):
    """The model could not be loaded or run for measurement."""


@dataclass(frozen=True)
class RunResult:
    backend: str                 # "cpu-host" (reference) | on-device backend later
    latency_ms: float            # median wall-clock per inference
    output_argmax: int | None
    peak_sram_kib: int | None    # None on host — on-device only
    power_mj: float | None       # None on host — on-board monitor read (HW-gated)
    runs: int


def run_host(onnx_path: Path, input_array: np.ndarray, *, runs: int = 20) -> RunResult:
    import onnxruntime as ort
    try:
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        name = sess.get_inputs()[0].name
        x = input_array.astype(np.float32)
        out = sess.run(None, {name: x})[0]          # warm-up + a real output
        times = []
        for _ in range(max(1, runs)):
            t0 = perf_counter()
            sess.run(None, {name: x})
            times.append((perf_counter() - t0) * 1000.0)
    except Exception as exc:
        raise MeasureError(f"host run failed: {exc}") from exc
    return RunResult(backend="cpu-host", latency_ms=round(median(times), 3),
                     output_argmax=int(np.asarray(out).ravel().argmax()),
                     peak_sram_kib=None, power_mj=None, runs=len(times))


def accuracy_vs(output_argmax: int | None, expected_label: int) -> bool:
    return output_argmax is not None and int(output_argmax) == int(expected_label)


@dataclass(frozen=True)
class ABComparison:
    faster: str                  # "a" | "b" | "tie"
    latency_ratio: float         # b.latency_ms / a.latency_ms
    a_latency_ms: float
    b_latency_ms: float
    size_delta_bytes: int | None  # size_b - size_a


def compare(a: RunResult, b: RunResult, *, size_a: int | None = None,
            size_b: int | None = None) -> ABComparison:
    if a.latency_ms < b.latency_ms:
        faster = "a"
    elif b.latency_ms < a.latency_ms:
        faster = "b"
    else:
        faster = "tie"
    ratio = (b.latency_ms / a.latency_ms) if a.latency_ms else float("inf")
    delta = (size_b - size_a) if (size_a is not None and size_b is not None) else None
    return ABComparison(faster=faster, latency_ratio=round(ratio, 4),
                        a_latency_ms=a.latency_ms, b_latency_ms=b.latency_ms,
                        size_delta_bytes=delta)


def estimate_vs_measured(est_latency_ms: float | None, measured_latency_ms: float) -> dict:
    """The self-improving loop: how far #1's static estimate was from reality.
    (Later) calibrates the tier-1 estimator. Decoupled — takes two numbers."""
    ratio = (measured_latency_ms / est_latency_ms) if est_latency_ms else None
    return {"est_latency_ms": est_latency_ms, "measured_latency_ms": measured_latency_ms,
            "ratio": round(ratio, 4) if ratio is not None else None}
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_model_measure.py -q`).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/measure.py tests/scripts/test_alp_model_measure.py
git commit -m "feat(model): measure engine — host run + A/B compare + estimate-vs-measured"
```

---

## Task 2: `alp model run`

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `run_host`, `accuracy_vs`, `MeasureError` (Task 1).
- Produces: `alp model run <model.onnx> [--input FILE.npy] [--expected LABEL] [--runs N] [--format]`. Without `--input`, a deterministic random input matching the model's first input shape is used (labelled `random_input: true`). JSON: `{"model","backend","latency_ms","output_argmax","peak_sram_kib","power_mj","runs","random_input",("accuracy": {"expected","match"})?,"note"}` where `note` states the host-reference caveat.

- [ ] **Step 1: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py`:

```python
def test_model_run_host_json(tmp_path):
    import json
    import numpy as np
    import pytest as _pytest
    _pytest.importorskip("onnxruntime")
    from click.testing import CliRunner
    from alp_cli.main import cli
    x = tmp_path / "in.npy"
    np.save(x, np.random.default_rng(0).standard_normal((1, 3, 224, 224)).astype(np.float32))
    raw = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    res = CliRunner().invoke(cli, ["model", "run", str(raw), "--input", str(x),
                                   "--runs", "3", "--format", "json"],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    p = json.loads(res.output)
    assert p["backend"] == "cpu-host" and p["latency_ms"] > 0
    assert p["power_mj"] is None and p["random_input"] is False
```

- [ ] **Step 2: Run → fail** (`no such command 'run'`).

- [ ] **Step 3: Implement `run_cmd`** in `scripts/alp_cli/model.py` (import + command after `prep_cmd`):

```python
import numpy as np
from alp_model.measure import MeasureError, accuracy_vs, run_host
```

```python
_HOST_NOTE = ("host reference run (backend=cpu-host): functional + host latency, "
              "NOT the target SoM. On-device latency/SRAM/power is the HW-gated follow-on.")


@model_group.command(name="run", help="Host reference run of a model (functional + host latency + accuracy).")
@click.argument("model", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--input", "input_path", default=None, type=click.Path(exists=True, path_type=Path),
              help="Input sample .npy (default: deterministic random matching the model input).")
@click.option("--expected", default=None, type=int, help="Expected class label (top-1 accuracy check).")
@click.option("--runs", default=20, show_default=True, help="Timed inference count (median latency).")
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def run_cmd(model: Path, input_path: Path | None, expected: int | None, runs: int, fmt: str) -> None:
    if model.suffix.lower() != ".onnx":
        click.echo(f"error: model run supports .onnx in this release; got {model.name}", err=True)
        raise SystemExit(1)
    import onnxruntime as ort
    sess = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    shape = [(1 if not isinstance(d, int) else d) for d in sess.get_inputs()[0].shape]
    random_input = input_path is None
    x = (np.load(input_path).astype(np.float32) if input_path is not None
         else np.random.default_rng(0).standard_normal(shape).astype(np.float32))
    try:
        r = run_host(model, x, runs=runs)
    except MeasureError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    payload = {"model": str(model), "backend": r.backend, "latency_ms": r.latency_ms,
               "output_argmax": r.output_argmax, "peak_sram_kib": r.peak_sram_kib,
               "power_mj": r.power_mj, "runs": r.runs, "random_input": random_input,
               "note": _HOST_NOTE}
    if expected is not None:
        payload["accuracy"] = {"expected": expected, "match": accuracy_vs(r.output_argmax, expected)}
    if fmt == "json":
        click.echo(json.dumps(payload, indent=2))
        return
    click.echo(f"model: {model}")
    click.echo(f"  [{r.backend}] latency {r.latency_ms} ms (median of {r.runs})  "
               f"argmax {r.output_argmax}  sram {r.peak_sram_kib}  power {r.power_mj}")
    if expected is not None:
        click.echo(f"  accuracy: top1 {'MATCH' if payload['accuracy']['match'] else 'MISS'} (expected {expected})")
    click.echo(f"  note: {_HOST_NOTE}")
```

- [ ] **Step 4: Run tests green** + human smoke (`PYTHONPATH=scripts py -3.11 -m alp_cli.main model run tests/fixtures/models/tiny_cnn.onnx --runs 3`).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model run' (host reference run)"
```

---

## Task 3: `alp model ab`

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `run_host`, `compare`, `MeasureError` (Task 1).
- Produces: `alp model ab <a.onnx> <b.onnx> [--input FILE.npy] [--runs N] [--format]`. Runs both on the host with the SAME input, emits `{"a":{...run...},"b":{...run...},"comparison":{"faster","latency_ratio","a_latency_ms","b_latency_ms","size_delta_bytes"},"note"}`.

- [ ] **Step 1: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py`:

```python
def test_model_ab_json(tmp_path):
    import json
    import numpy as np
    import pytest as _pytest
    _pytest.importorskip("onnxruntime")
    from click.testing import CliRunner
    from alp_cli.main import cli
    x = tmp_path / "in.npy"
    np.save(x, np.random.default_rng(0).standard_normal((1, 3, 224, 224)).astype(np.float32))
    raw = str(_ROOT / "tests/fixtures/models/tiny_cnn.onnx")
    res = CliRunner().invoke(cli, ["model", "ab", raw, raw, "--input", str(x),
                                   "--runs", "3", "--format", "json"],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    p = json.loads(res.output)
    assert p["comparison"]["faster"] in ("a", "b", "tie")
    assert p["comparison"]["size_delta_bytes"] == 0     # same file
```

- [ ] **Step 2: Run → fail** (`no such command 'ab'`).

- [ ] **Step 3: Implement `ab_cmd`** in `scripts/alp_cli/model.py` (after `run_cmd`; add `compare` to the measure import):

```python
from alp_model.measure import MeasureError, accuracy_vs, compare, run_host
```

```python
@model_group.command(name="ab", help="A/B two models on the same input (host reference): latency + size.")
@click.argument("model_a", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.argument("model_b", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--input", "input_path", default=None, type=click.Path(exists=True, path_type=Path),
              help="Shared input sample .npy (default: deterministic random matching model_a's input).")
@click.option("--runs", default=20, show_default=True)
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def ab_cmd(model_a: Path, model_b: Path, input_path: Path | None, runs: int, fmt: str) -> None:
    for m in (model_a, model_b):
        if m.suffix.lower() != ".onnx":
            click.echo(f"error: model ab supports .onnx in this release; got {m.name}", err=True)
            raise SystemExit(1)
    import onnxruntime as ort
    sess = ort.InferenceSession(str(model_a), providers=["CPUExecutionProvider"])
    shape = [(1 if not isinstance(d, int) else d) for d in sess.get_inputs()[0].shape]
    x = (np.load(input_path).astype(np.float32) if input_path is not None
         else np.random.default_rng(0).standard_normal(shape).astype(np.float32))
    try:
        ra = run_host(model_a, x, runs=runs)
        rb = run_host(model_b, x, runs=runs)
    except MeasureError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    cmp = compare(ra, rb, size_a=model_a.stat().st_size, size_b=model_b.stat().st_size)

    def _run(r: "object") -> dict:
        return {"backend": r.backend, "latency_ms": r.latency_ms,
                "output_argmax": r.output_argmax, "runs": r.runs,
                "peak_sram_kib": r.peak_sram_kib, "power_mj": r.power_mj}

    payload = {"a": {"model": str(model_a), **_run(ra)}, "b": {"model": str(model_b), **_run(rb)},
               "comparison": {"faster": cmp.faster, "latency_ratio": cmp.latency_ratio,
                              "a_latency_ms": cmp.a_latency_ms, "b_latency_ms": cmp.b_latency_ms,
                              "size_delta_bytes": cmp.size_delta_bytes},
               "note": _HOST_NOTE}
    if fmt == "json":
        click.echo(json.dumps(payload, indent=2))
        return
    click.echo(f"A {model_a.name}: {ra.latency_ms} ms   B {model_b.name}: {rb.latency_ms} ms")
    click.echo(f"  faster: {cmp.faster}  (B/A latency x{cmp.latency_ratio})  "
               f"size delta {cmp.size_delta_bytes} B")
    click.echo(f"  note: {_HOST_NOTE}")
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k "run or ab"`).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model ab' (host A/B compare)"
```

---

## Task 4: Docs

**Files:**
- Modify: `docs/cli.md`

- [ ] **Step 1: Document `model run` + `model ab`** (same style as siblings): synopses; that `run`/`ab` are HOST reference runs (`backend: cpu-host`, functional + host-latency, NOT target-SoM performance); the JSON payloads; that `peak_sram_kib`/`power_mj` are `None` on host and populated only by the HW-gated on-device runner (per-rail energy via the on-board shunt/monitor IC); the estimate-vs-measured calibration loop as the intended use of measured latency vs `alp model check`'s estimate.

- [ ] **Step 2: Doc-drift gate** (`py -3.11 scripts/check_doc_drift.py` → OK).

- [ ] **Step 3: Commit**

```bash
git add docs/
git commit -m "docs(model): document model run/ab (host reference) + HW-gated on-device"
```

---

## Self-Review

- **Spec coverage** (roadmap §4 sub-project 4): run/observe → `model run` (host reference; on-device HW-gated) ✓; telemetry schema (`RunResult`: latency/backend/argmax/sram/power) ✓; A/B compare → `model ab` ✓; self-improving loop → `estimate_vs_measured` ✓. Deferred (HW/runtime-gated + flagged): on-device run + per-rail power via on-board monitor; the A55 DEEPX/DRP-AI path (unshipped Yocto NPU runtimes); the extension run/A-B panel + fleet dashboard; the tan `model run`/`ab` wrappers.
- **Placeholder scan:** all code complete; `power_mj`/`peak_sram_kib` are deliberate `None` (HW-only), documented, not placeholders.
- **Type consistency:** `RunResult`/`ABComparison` fields identical across engine, CLI, tests; `run_host(onnx, arr, *, runs)` + `compare(a, b, *, size_a, size_b)` signatures identical in engine + CLI; `MeasureError` raised in engine, caught in CLI.
- **Honesty:** every host result is labelled `cpu-host` + carries the `note` caveat; no host number is presented as a target-SoM number; power/SRAM left `None` until a real on-device read.
