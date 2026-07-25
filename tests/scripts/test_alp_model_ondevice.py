"""alp_model.ondevice -- console-capture parsing + capture-to-EnergyMeasurement
conversion (the IMPURE half; see test_alp_model_measure.py for the pure math).
All hermetic: no hardware, no subprocess -- capture text is built in-process."""
import json
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))

_DEFAULT_CFG = {
    "rail": "+3V3", "addr": 64, "shunt_ohms": 0.02, "current_lsb_a": 0.00012207,
    "power_lsb_w": 0.001, "adcrange": 0, "avg": 4, "vbusct_us": 140, "vshct_us": 140,
    "sample_period_us": 1000, "cycles_per_s": 1_000_000, "n_inferences": 1,
    "windows": 1, "npu_dispatched": False,
}


def _capture_text(cfg=None, windows=None, energy_w=None, device_result=None,
                   header=True, banner=True):
    """Assemble a synthetic on-target console capture from structured data.
    windows: {window_index: {"active"|"idle": [(cycles, power_raw), ...]}}
    energy_w: {window_index: {"active"|"idle": (n_samples, span_cycles, span_ms)}}"""
    lines = []
    if banner:
        lines += ["*** Booting Zephyr OS build v4.4.0 ***", "some other printk noise"]
    if header:
        lines.append("ENERGY-CFG " + json.dumps({**_DEFAULT_CFG, **(cfg or {})}))
    for w_i, phases in (windows or {}).items():
        for phase, points in phases.items():
            for cycles, power_raw in points:
                lines.append(f"ENERGY-S {w_i} {phase} {cycles} {power_raw}")
    for w_i, phases in (energy_w or {}).items():
        for phase, (n, span_cycles, span_ms) in phases.items():
            lines.append(f"ENERGY-W {w_i} {phase} {n} {span_cycles} {span_ms}")
    if device_result is not None:
        lines.append("ENERGY-RESULT " + json.dumps(device_result))
    if banner:
        lines.append("[00:00:01.234,000] <inf> app: idle thread running")
    return "\n".join(lines) + "\n"


def test_parse_console_ignores_interleaved_banner_noise():
    from alp_model.ondevice import parse_console
    text = _capture_text(
        cfg={"n_inferences": 10},
        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                     "idle": [(0, 1000), (1_000_000, 1000)]}},
        banner=True)
    parsed = parse_console(text)
    assert parsed.cfg["rail"] == "+3V3"
    assert parsed.cfg["n_inferences"] == 10
    assert parsed.samples[0]["active"] == [(0, 3000), (1_000_000, 3000)]
    assert parsed.samples[0]["idle"] == [(0, 1000), (1_000_000, 1000)]


def test_measurement_from_capture_constant_power_known_exactly():
    # active: 3.0 W (power_raw=3000, power_lsb_w=0.001) held for 2.0 s (2_000_000
    # cycles @ 1_000_000 cycles/s) -> 6.0 J = 6000 mJ.
    # idle:   1.0 W held for the same 2.0 s -> 2.0 J = 2000 mJ.
    # excess = 4000 mJ over n_inferences=10 -> 400.0 mJ/inference exactly.
    from alp_model.ondevice import measurement_from_capture, parse_console
    text = _capture_text(
        cfg={"n_inferences": 10},
        windows={0: {"active": [(0, 3000), (2_000_000, 3000)],
                     "idle": [(0, 1000), (2_000_000, 1000)]}},
        banner=False)
    m = measurement_from_capture(parse_console(text))
    assert m.value_mj_per_inference == pytest.approx(400.0)
    assert m.rails == ["+3V3"]
    assert m.n_inferences == 10
    assert m.window_ms == pytest.approx(2000.0)
    assert m.sample_count == 4
    assert m.spread_mj is None


def test_measurement_from_capture_identical_windows_is_zero():
    from alp_model.ondevice import measurement_from_capture, parse_console
    window = [(0, 2000), (1_000_000, 2000)]
    text = _capture_text(cfg={"n_inferences": 5},
                         windows={0: {"active": window, "idle": window}}, banner=False)
    m = measurement_from_capture(parse_console(text))
    assert m.value_mj_per_inference == pytest.approx(0.0)


def test_cycles_wrap_mid_window_gives_correct_positive_duration():
    # active samples wrap the uint32 counter: first=2**32-1000, second=500.
    # Unsigned delta = (500 - (2**32-1000)) % 2**32 = 1500 cycles ->
    # 1500 / 1_000_000 cycles/s = 0.0015 s = 1.5 ms -- small, correct, positive.
    from alp_model.ondevice import measurement_from_capture, parse_console
    wrap_point = (1 << 32) - 1000
    text = _capture_text(
        cfg={"n_inferences": 1},
        windows={0: {"active": [(wrap_point, 1000), (500, 1000)],
                     "idle": [(0, 1000), (3000, 1000)]}},
        banner=False)
    m = measurement_from_capture(parse_console(text))
    assert m.window_ms == pytest.approx(1.5)


@pytest.mark.parametrize("text", [
    # missing header entirely
    "ENERGY-S 0 active 0 1000\nENERGY-S 0 active 1000 1000\n",
    # malformed header (broken JSON)
    "ENERGY-CFG {not json}\nENERGY-S 0 active 0 1000\n",
])
def test_parse_console_bad_or_missing_header_raises(text):
    from alp_model.ondevice import OnDeviceError, parse_console
    with pytest.raises(OnDeviceError):
        parse_console(text)


def test_parse_console_zero_windows_raises():
    from alp_model.ondevice import OnDeviceError, parse_console
    text = _capture_text(windows=None, banner=False)  # header only, no ENERGY-S
    with pytest.raises(OnDeviceError):
        parse_console(text)


def test_parse_console_one_sample_window_raises():
    from alp_model.ondevice import OnDeviceError, parse_console
    text = _capture_text(
        windows={0: {"active": [(0, 1000)],       # only 1 sample -- invalid
                     "idle": [(0, 1000), (1000, 1000)]}},
        banner=False)
    with pytest.raises(OnDeviceError):
        parse_console(text)


def _three_window_capture(cfg_overrides=None, energy_w=None, device_result=None):
    # Each window: 1 s (1_000_000 cycles @ 1_000_000 cycles/s) of active vs.
    # idle at a distinct constant delta-power, n_inferences=1 so
    # mJ/inference == the window's raw mJ delta:
    #   w0: 3.0W active vs 1.0W idle -> 2.0 W * 1 s * 1000 = 2000 mJ
    #   w1: 4.0W active vs 1.0W idle -> 3.0 W * 1 s * 1000 = 3000 mJ
    #   w2: 5.0W active vs 1.0W idle -> 4.0 W * 1 s * 1000 = 4000 mJ
    # mean = 3000.0 mJ; sample stdev([2000,3000,4000]) = 1000.0
    windows = {
        0: {"active": [(0, 3000), (1_000_000, 3000)], "idle": [(0, 1000), (1_000_000, 1000)]},
        1: {"active": [(0, 4000), (1_000_000, 4000)], "idle": [(0, 1000), (1_000_000, 1000)]},
        2: {"active": [(0, 5000), (1_000_000, 5000)], "idle": [(0, 1000), (1_000_000, 1000)]},
    }
    return _capture_text(cfg={"n_inferences": 1, **(cfg_overrides or {})}, windows=windows,
                         energy_w=energy_w, device_result=device_result, banner=False)


def test_spread_mj_none_for_one_window_and_stdev_for_three():
    from alp_model.ondevice import measurement_from_capture, parse_console
    # one window: spread must be None (nothing to spread across)
    one = _capture_text(cfg={"n_inferences": 1},
                        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                                     "idle": [(0, 1000), (1_000_000, 1000)]}},
                        banner=False)
    m1 = measurement_from_capture(parse_console(one))
    assert m1.spread_mj is None

    # three windows: mean 3000.0, sample stdev 1000.0 (hand-computed above)
    m3 = measurement_from_capture(parse_console(_three_window_capture()))
    assert m3.value_mj_per_inference == pytest.approx(3000.0)
    assert m3.spread_mj == pytest.approx(1000.0)
    assert m3.window_ms == pytest.approx(1000.0)
    assert m3.sample_count == 12          # 3 windows * (2 active + 2 idle)


def test_cycles_per_s_reconciliation_prefers_measured_over_dt():
    # cfg deliberately carries a WRONG DT constant (500_000); every ENERGY-W
    # span agrees on the true 1_000_000 cycles/s rate (1_000_000 cycles /
    # 1000 ms * 1000 = 1_000_000) -- self-consistent, so the measured rate
    # must win, and diagnostics must report both.
    from alp_model.ondevice import capture_diagnostics, parse_console
    energy_w = {
        0: {"active": (2, 1_000_000, 1000.0), "idle": (2, 1_000_000, 1000.0)},
        1: {"active": (2, 1_000_000, 1000.0), "idle": (2, 1_000_000, 1000.0)},
        2: {"active": (2, 1_000_000, 1000.0), "idle": (2, 1_000_000, 1000.0)},
    }
    text = _three_window_capture(cfg_overrides={"cycles_per_s": 500_000}, energy_w=energy_w)
    diag = capture_diagnostics(parse_console(text))
    assert diag["cycles_per_s_dt"] == pytest.approx(500_000.0)
    assert diag["cycles_per_s_measured"] == pytest.approx(1_000_000.0)
    assert diag["cycles_per_s_used"] == pytest.approx(1_000_000.0)
    assert diag["windows"] == [0, 1, 2]
    assert diag["npu_dispatched"] is False


def test_capture_diagnostics_device_vs_host_ratio():
    from alp_model.ondevice import capture_diagnostics, parse_console
    text = _three_window_capture(device_result={"value_mj_per_inference": 1500.0, "spread_mj": 10.0})
    diag = capture_diagnostics(parse_console(text))
    assert diag["device_value_mj_per_inference"] == pytest.approx(1500.0)
    # host mean is 3000.0 mJ/inference (see _three_window_capture) -> ratio 2.0
    assert diag["host_vs_device_ratio"] == pytest.approx(2.0)


def test_measurement_from_capture_keeps_honest_labels():
    from alp_model.ondevice import measurement_from_capture, parse_console
    m = measurement_from_capture(parse_console(_three_window_capture()))
    assert m.source == "measured"
    assert m.scope == "carrier-rail-delta"


# ---------------------------------------------------------------------------
# CLI: --on-device failure must exit non-zero and never emit a null energy
# (a silent null would read as "measured nothing" instead of "run failed").
# ---------------------------------------------------------------------------

def test_cli_on_device_failure_exits_nonzero_without_null_energy(monkeypatch):
    pytest.importorskip("onnxruntime")
    from click.testing import CliRunner

    import alp_cli.model as climod
    from alp_cli.main import cli
    from alp_model.ondevice import OnDeviceError

    def _boom(app_dir, **kwargs):
        raise OnDeviceError("labgrid place 'aen-e8-01' has no held reservation")

    monkeypatch.setattr(climod, "resolve_app_dir", lambda *a, **k: Path("dummy"))
    monkeypatch.setattr(climod, "run_on_device", _boom)

    raw = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    res = CliRunner().invoke(cli, ["model", "run", str(raw), "--runs", "3",
                                   "--on-device", "--format", "json"])
    assert res.exit_code != 0
    assert "error:" in res.output
    assert "no held reservation" in res.output
    assert "energy" not in res.output       # failed run never reaches payload emission
