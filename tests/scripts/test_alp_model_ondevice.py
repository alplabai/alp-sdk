"""alp_model.ondevice -- console-capture parsing + capture-to-EnergyMeasurement
conversion (the IMPURE half; see test_alp_model_measure.py for the pure math).
All hermetic: no hardware, no subprocess -- capture text is built in-process.

Every fixture line format here is transcribed from the firmware's actual
printk calls in examples/aen/aen-inference-energy/src/main.cpp -- a reader
should be able to diff a fixture against that file's printk strings and see
them match. In particular: ENERGY-CFG carries NO "n_inferences" key (the
firmware never emits one -- it only appears per-window in ENERGY-W's 6th
field), which is the exact contract mismatch that used to raise an uncaught
KeyError against a real device capture."""
import json
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))

# Matches ENERGY-CFG's real key set exactly (main.cpp's printk("ENERGY-CFG ...")):
# rail, addr, shunt_ohms, current_lsb_a, power_lsb_w, adcrange, avg, vbusct_us,
# vshct_us, sample_period_us, cycles_per_s, samples_per_window, windows,
# npu_dispatched, baseline, rail_selected_by, timestamp_source. No other key
# exists on this line -- in particular no "n_inferences".
_DEFAULT_CFG = {
    "rail": "+3V3", "addr": 64, "shunt_ohms": 0.02, "current_lsb_a": 0.00012207,
    "power_lsb_w": 0.001, "adcrange": 1, "avg": 16, "vbusct_us": 140, "vshct_us": 140,
    "sample_period_us": 4480, "cycles_per_s": 1_000_000, "samples_per_window": 250,
    "windows": 3, "npu_dispatched": True, "baseline": "cpu-spin-npu-idle",
    "rail_selected_by": "largest-significant-delta", "timestamp_source": "dwt-cyccnt",
}


def _capture_text(cfg=None, windows=None, energy_w=None, device_result=None,
                   header=True, banner=True, extra_lines=None):
    """Assemble a synthetic on-target console capture from structured data.
    windows: {window_index: {"active"|"idle": [(cycles, power_raw), ...]}}
    energy_w: {window_index: {"active"|"idle": (n_samples, span_cycles, span_ms, inferences)}}
    extra_lines: raw ENERGY-WPART/ENERGY-WERR/ENERGY-WARN/ENERGY-SCAN lines to
    inject verbatim, for the diagnostic-line tolerance tests."""
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
        for phase, (n, span_cycles, span_ms, inferences) in phases.items():
            lines.append(f"ENERGY-W {w_i} {phase} {n} {span_cycles} {span_ms} {inferences}")
    for line in (extra_lines or []):
        lines.append(line)
    if device_result is not None:
        lines.append("ENERGY-RESULT " + json.dumps(device_result))
    if banner:
        lines.append("[00:00:01.234,000] <inf> app: idle thread running")
    return "\n".join(lines) + "\n"


def test_parse_console_ignores_interleaved_banner_noise():
    from alp_model.ondevice import parse_console
    text = _capture_text(
        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                     "idle": [(0, 1000), (1_000_000, 1000)]}},
        energy_w={0: {"active": (2, 1_000_000, 1000.0, 10), "idle": (2, 1_000_000, 1000.0, 0)}},
        banner=True)
    parsed = parse_console(text)
    assert parsed.cfg["rail"] == "+3V3"
    assert "n_inferences" not in parsed.cfg  # the firmware never emits this key
    assert parsed.samples[0]["active"] == [(0, 3000), (1_000_000, 3000)]
    assert parsed.samples[0]["idle"] == [(0, 1000), (1_000_000, 1000)]


def test_measurement_from_capture_constant_power_known_exactly():
    # active: 3.0 W (power_raw=3000, power_lsb_w=0.001) held for 2.0 s (2_000_000
    # cycles @ 1_000_000 cycles/s) -> 6.0 J = 6000 mJ.
    # idle:   1.0 W held for the same 2.0 s -> 2.0 J = 2000 mJ.
    # excess = 4000 mJ over the ENERGY-W-reported active inferences=10 -> 400.0
    # mJ/inference exactly.
    from alp_model.ondevice import measurement_from_capture, parse_console
    text = _capture_text(
        windows={0: {"active": [(0, 3000), (2_000_000, 3000)],
                     "idle": [(0, 1000), (2_000_000, 1000)]}},
        energy_w={0: {"active": (2, 2_000_000, 2000.0, 10), "idle": (2, 2_000_000, 2000.0, 0)}},
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
    text = _capture_text(
        windows={0: {"active": window, "idle": window}},
        energy_w={0: {"active": (2, 1_000_000, 1000.0, 5), "idle": (2, 1_000_000, 1000.0, 0)}},
        banner=False)
    m = measurement_from_capture(parse_console(text))
    assert m.value_mj_per_inference == pytest.approx(0.0)


def test_cycles_wrap_mid_window_gives_correct_positive_duration():
    # active samples wrap the uint32 counter: first=2**32-1000, second=500.
    # Unsigned delta = (500 - (2**32-1000)) % 2**32 = 1500 cycles ->
    # 1500 / 1_000_000 cycles/s = 0.0015 s = 1.5 ms -- small, correct, positive.
    from alp_model.ondevice import measurement_from_capture, parse_console
    wrap_point = (1 << 32) - 1000
    text = _capture_text(
        windows={0: {"active": [(wrap_point, 1000), (500, 1000)],
                     "idle": [(0, 1000), (3000, 1000)]}},
        energy_w={0: {"active": (2, 1500, 1.5, 1), "idle": (2, 3000, 3.0, 0)}},
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


def _three_window_capture(cfg_overrides=None, energy_w=None, device_result=None,
                          active_inferences=1):
    # Each window: 1 s (1_000_000 cycles @ 1_000_000 cycles/s) of active vs.
    # idle at a distinct constant delta-power, active inferences=1 (via
    # ENERGY-W) so mJ/inference == the window's raw mJ delta:
    #   w0: 3.0W active vs 1.0W idle -> 2.0 W * 1 s * 1000 = 2000 mJ
    #   w1: 4.0W active vs 1.0W idle -> 3.0 W * 1 s * 1000 = 3000 mJ
    #   w2: 5.0W active vs 1.0W idle -> 4.0 W * 1 s * 1000 = 4000 mJ
    # mean = 3000.0 mJ; sample stdev([2000,3000,4000]) = 1000.0
    windows = {
        0: {"active": [(0, 3000), (1_000_000, 3000)], "idle": [(0, 1000), (1_000_000, 1000)]},
        1: {"active": [(0, 4000), (1_000_000, 4000)], "idle": [(0, 1000), (1_000_000, 1000)]},
        2: {"active": [(0, 5000), (1_000_000, 5000)], "idle": [(0, 1000), (1_000_000, 1000)]},
    }
    default_energy_w = {
        w: {"active": (2, 1_000_000, 1000.0, active_inferences), "idle": (2, 1_000_000, 1000.0, 0)}
        for w in windows
    }
    return _capture_text(cfg=cfg_overrides, windows=windows,
                         energy_w=energy_w if energy_w is not None else default_energy_w,
                         device_result=device_result, banner=False)


def test_spread_mj_none_for_one_window_and_stdev_for_three():
    from alp_model.ondevice import measurement_from_capture, parse_console
    # one window: spread must be None (nothing to spread across)
    one = _capture_text(
        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                     "idle": [(0, 1000), (1_000_000, 1000)]}},
        energy_w={0: {"active": (2, 1_000_000, 1000.0, 1), "idle": (2, 1_000_000, 1000.0, 0)}},
        banner=False)
    m1 = measurement_from_capture(parse_console(one))
    assert m1.spread_mj is None

    # three windows: mean 3000.0, sample stdev 1000.0 (hand-computed above)
    m3 = measurement_from_capture(parse_console(_three_window_capture()))
    assert m3.value_mj_per_inference == pytest.approx(3000.0)
    assert m3.spread_mj == pytest.approx(1000.0)
    assert m3.window_ms == pytest.approx(1000.0)
    assert m3.sample_count == 12          # 3 windows * (2 active + 2 idle)


def test_energy_w_six_field_per_window_inference_count_is_the_divisor():
    # The BLOCKER this test guards: n_inferences is measured PER WINDOW and
    # differs between windows -- it is not a single config-wide constant, so
    # ENERGY-CFG never carries it and the divisor must come from each pair's
    # own ENERGY-W line.
    #   w0: delta = (3.0-1.0)W * 1s * 1000 = 2000 mJ, active inferences=2 -> 1000.0 mJ/inference
    #   w1: delta = (3.0-1.0)W * 1s * 1000 = 2000 mJ, active inferences=4 -> 500.0 mJ/inference
    # mean = 750.0; sample stdev([1000.0, 500.0]) = 353.5533905932738
    # total_inferences = 2 + 4 = 6
    from alp_model.ondevice import measurement_from_capture, parse_console
    windows = {
        0: {"active": [(0, 3000), (1_000_000, 3000)], "idle": [(0, 1000), (1_000_000, 1000)]},
        1: {"active": [(0, 3000), (1_000_000, 3000)], "idle": [(0, 1000), (1_000_000, 1000)]},
    }
    energy_w = {
        0: {"active": (2, 1_000_000, 1000.0, 2), "idle": (2, 1_000_000, 1000.0, 0)},
        1: {"active": (2, 1_000_000, 1000.0, 4), "idle": (2, 1_000_000, 1000.0, 0)},
    }
    text = _capture_text(windows=windows, energy_w=energy_w, banner=False)
    m = measurement_from_capture(parse_console(text))
    assert m.value_mj_per_inference == pytest.approx(750.0)
    assert m.spread_mj == pytest.approx(353.5533905932738)
    assert m.n_inferences == 6


def test_energy_w_five_field_old_firmware_raises_clear_ondevice_error():
    # An older image that predates per-window inference reporting emits the
    # 5-field ENERGY-W (no trailing <inferences>). That must be a clear,
    # named OnDeviceError -- never a bare KeyError/IndexError from code that
    # assumed the 6th field exists.
    from alp_model.ondevice import OnDeviceError, parse_console
    text = _capture_text(
        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                     "idle": [(0, 1000), (1_000_000, 1000)]}},
        banner=False) + "ENERGY-W 0 active 2 1000000 1000.0\n"
    with pytest.raises(OnDeviceError, match="5 fields"):
        parse_console(text)


def test_energy_wpart_raises_ondevice_error_not_silent_partial_energy():
    # A partial sample stream (device buffer shorter than what it integrated)
    # must never be silently re-integrated from the truncated stream -- the
    # host cannot recover the missing samples, so it must refuse, not guess.
    from alp_model.ondevice import OnDeviceError, parse_console
    text = _capture_text(
        windows={0: {"active": [(0, 3000), (1_000_000, 3000)],
                     "idle": [(0, 1000), (1_000_000, 1000)]}},
        energy_w={0: {"active": (2, 1_000_000, 1000.0, 1), "idle": (2, 1_000_000, 1000.0, 0)}},
        extra_lines=["ENERGY-WPART 0 active emitted=2 of 250 samples (device integral is "
                     "authoritative for this build)"],
        banner=False)
    with pytest.raises(OnDeviceError, match="partial"):
        parse_console(text)


def test_energy_werr_and_warn_and_npu_dispatched_false_surfaced_in_diagnostics():
    # A degraded run (I2C errors, a timed-out window, NPU dispatch failure)
    # must stay visible in diagnostics rather than looking identical to a
    # clean run.
    from alp_model.ondevice import capture_diagnostics, parse_console
    werr = "ENERGY-WERR 0 active timed_out=1 i2c_errors=3 last_rc=-5 got=120/250"
    warn = ("ENERGY-WARN active window 30000 ms exceeds the cycle-counter wrap -- "
            "span_cycles is not meaningful; lower AEN_ENERGY_SAMPLES_PER_WINDOW")
    text = _three_window_capture(cfg_overrides={"npu_dispatched": False})
    # _three_window_capture has no extra_lines hook of its own -- append the
    # diagnostic lines directly; parse_console reads them the same either way.
    text = text.rstrip("\n") + f"\n{werr}\n{warn}\n"
    diag = capture_diagnostics(parse_console(text))
    assert diag["npu_dispatched"] is False
    assert werr in diag["werr_lines"]
    assert warn in diag["warn_lines"]


def test_cycles_per_s_reconciliation_prefers_measured_over_dt():
    # cfg deliberately carries a WRONG DT constant (500_000); every ENERGY-W
    # span agrees on the true 1_000_000 cycles/s rate (1_000_000 cycles /
    # 1000 ms * 1000 = 1_000_000) -- self-consistent, so the measured rate
    # must win, and diagnostics must report both.
    from alp_model.ondevice import capture_diagnostics, parse_console
    energy_w = {
        0: {"active": (2, 1_000_000, 1000.0, 1), "idle": (2, 1_000_000, 1000.0, 0)},
        1: {"active": (2, 1_000_000, 1000.0, 1), "idle": (2, 1_000_000, 1000.0, 0)},
        2: {"active": (2, 1_000_000, 1000.0, 1), "idle": (2, 1_000_000, 1000.0, 0)},
    }
    text = _three_window_capture(cfg_overrides={"cycles_per_s": 500_000}, energy_w=energy_w)
    diag = capture_diagnostics(parse_console(text))
    assert diag["cycles_per_s_dt"] == pytest.approx(500_000.0)
    assert diag["cycles_per_s_measured"] == pytest.approx(1_000_000.0)
    assert diag["cycles_per_s_used"] == pytest.approx(1_000_000.0)
    assert diag["windows"] == [0, 1, 2]
    assert diag["npu_dispatched"] is True


def test_capture_diagnostics_device_vs_host_ratio():
    from alp_model.ondevice import capture_diagnostics, parse_console
    device_result = {
        "source": "measured", "scope": "carrier-rail-delta", "value_mj_per_inference": 1500.0,
        "rails": ["+3V3"], "n_inferences": 1, "window_ms": 1000.0, "sample_count": 12,
        "pairs_used": 3, "total_inferences": 3, "spread_mj": 10.0,
    }
    text = _three_window_capture(device_result=device_result)
    diag = capture_diagnostics(parse_console(text))
    assert diag["device_value_mj_per_inference"] == pytest.approx(1500.0)
    # host mean is 3000.0 mJ/inference (see _three_window_capture) -> ratio 2.0
    assert diag["host_vs_device_ratio"] == pytest.approx(2.0)


def test_device_result_spread_mj_null_parses_as_none():
    # The firmware emits JSON `null` for a single-pair run, not the old
    # "-0.000000" sentinel (json.loads(-0.000000) == -0.0, and -0.0 < 0 is
    # False in Python, so every reader used to see 0.0 -- the exact
    # misreading `null` exists to prevent).
    from alp_model.ondevice import parse_console
    device_result = {
        "source": "measured", "scope": "carrier-rail-delta", "value_mj_per_inference": 3000.0,
        "rails": ["+3V3"], "n_inferences": 1, "window_ms": 1000.0, "sample_count": 12,
        "pairs_used": 3, "total_inferences": 3, "spread_mj": None,
    }
    text = _three_window_capture(device_result=device_result)
    parsed = parse_console(text)
    assert parsed.device_result["spread_mj"] is None
    assert parsed.device_result["spread_mj"] != -0.0


def test_measurement_from_capture_keeps_honest_labels():
    from alp_model.ondevice import measurement_from_capture, parse_console
    m = measurement_from_capture(parse_console(_three_window_capture()))
    assert m.source == "measured"
    assert m.scope == "carrier-rail-delta"
