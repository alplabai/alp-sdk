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


def test_integrate_energy_constant_power_known_duration():
    # 2.0 W held for 3.0 s -> 6.0 J = 6000.0 mJ, exactly (constant power makes
    # trapezoidal == rectangular, so this also pins the units/scale).
    from alp_model.measure import integrate_energy
    samples = [(0.0, 2.0), (1.0, 2.0), (3.0, 2.0)]
    assert integrate_energy(samples) == pytest.approx(6000.0)


def test_integrate_energy_under_two_samples_is_zero():
    from alp_model.measure import integrate_energy
    assert integrate_energy([]) == 0.0
    assert integrate_energy([(0.0, 5.0)]) == 0.0


def test_windowed_delta_identical_windows_cancels_to_zero():
    from alp_model.measure import windowed_delta
    window = [(0.0, 1.5), (1.0, 1.5), (2.0, 1.5)]
    assert windowed_delta(window, window, n_inferences=10) == pytest.approx(0.0)


def test_windowed_delta_divides_excess_energy_by_n_inferences():
    from alp_model.measure import windowed_delta
    # active: 3.0 W for 2.0 s = 6000 mJ; idle: 1.0 W for 2.0 s = 2000 mJ.
    # excess = 4000 mJ over 8 inferences -> 500 mJ/inference.
    active = [(0.0, 3.0), (2.0, 3.0)]
    idle = [(0.0, 1.0), (2.0, 1.0)]
    assert windowed_delta(active, idle, n_inferences=8) == pytest.approx(500.0)


def test_windowed_delta_rejects_zero_inferences():
    from alp_model.measure import windowed_delta
    with pytest.raises(ValueError):
        windowed_delta([(0.0, 1.0), (1.0, 1.0)], [(0.0, 1.0), (1.0, 1.0)], n_inferences=0)


def test_energy_measurement_carries_honest_labels():
    from alp_model.measure import EnergyMeasurement
    m = EnergyMeasurement(value_mj_per_inference=12.3, rails=["+3V3"], n_inferences=8,
                          window_ms=100.0, sample_count=50)
    assert m.source == "measured"
    assert m.scope == "carrier-rail-delta"
    assert m.spread_mj is None


def test_energy_measurement_rejects_non_honest_scope():
    from alp_model.measure import EnergyMeasurement
    with pytest.raises(ValueError):
        EnergyMeasurement(value_mj_per_inference=1.0, rails=["+3V3"], n_inferences=1,
                          window_ms=10.0, sample_count=2, scope="npu-only")


def test_energy_measurement_rejects_non_honest_source():
    from alp_model.measure import EnergyMeasurement
    with pytest.raises(ValueError):
        EnergyMeasurement(value_mj_per_inference=1.0, rails=["+3V3"], n_inferences=1,
                          window_ms=10.0, sample_count=2, source="estimated")


def test_run_host_energy_is_honestly_none():
    pytest.importorskip("onnxruntime")
    from alp_model.measure import run_host
    x = np.random.default_rng(0).standard_normal((1, 3, 224, 224)).astype(np.float32)
    r = run_host(_ONNX, x, runs=3)
    assert r.energy is None
