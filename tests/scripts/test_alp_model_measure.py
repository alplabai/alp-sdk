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
