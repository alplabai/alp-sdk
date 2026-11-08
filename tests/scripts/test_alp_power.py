# SPDX-License-Identifier: Apache-2.0
import json
import signal
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
from alp_power import analysis, capture, cli  # noqa: E402
from alp_power.analysis import Sample  # noqa: E402
from alp_power.monitors import Ina236, parse_monitor  # noqa: E402
from alp_power.transport import (ProbeError, dec_stream_read,  # noqa: E402
                                 enc_stream_config, unwrap_ts)

FIXTURE = REPO / "tests/scripts/fixtures/alp_power/synthetic_capture.jsonl"


def run(*args):
    return subprocess.run([sys.executable, str(REPO / "scripts/alp_power.py"), *args],
                          capture_output=True, text=True, encoding="utf-8")


# --- INA236 maths ----------------------------------------------------------
def test_ina236_20mohm_fine_range():
    m = Ina236(0x4A, 0.02, "fine")
    assert m.cal == 2048
    assert m.current_lsb == pytest.approx(31.25e-6)
    assert m.configure() == [bytes([0, 0x50, 0x07]), bytes([5, 0x08, 0x00])]
    assert m.decode(bytes([0x03, 0xE8])) == {"watts": pytest.approx(1.0)}  # 1000 x 1 mW


def test_ina236_20mohm_wide_range():
    m = Ina236(0x4A, 0.02, "wide")
    assert m.cal == 2048
    assert m.current_lsb == pytest.approx(125e-6)
    assert m.configure()[0] == bytes([0, 0x40, 0x07])
    assert m.decode(bytes([0x00, 0x64]))["watts"] == pytest.approx(0.4)  # 100 x 4 mW


def test_ina236_current_and_bus_decode():
    m = Ina236(0x4A, 0.02, "fine")
    assert m.decode_reg(0x04, bytes([0xFF, 0xFF])) == ("amps", pytest.approx(-31.25e-6))
    assert m.decode_reg(0x02, bytes([0x0C, 0x80])) == ("volts", pytest.approx(5.12))
    assert m.stream_channels() == [(0x4A, 0x03, 2)]


def test_monitor_spec_parse_and_reject():
    name, m = parse_monitor("5V=ina236@0x4A,shunt=0.02,range=fine")
    assert (name, m.addr, m.range) == ("5V", 0x4A, "fine")
    for bad in ("x=ina999@0x4A,shunt=0.02", "x=ina236@0x4A", "x=ina236@0x90,shunt=1",
                "x=ina236@0x4A,shunt=0.02,range=huge"):
        with pytest.raises(ValueError):
            parse_monitor(bad)


# --- wire / timestamps -------------------------------------------------------
def test_unwrap_ts():
    rows = [(0xFFFFFFF0, 0, b""), (0x10, 0, b""), (0x20, 0, b"")]
    assert [r[0] for r in unwrap_ts(rows)] == [0xFFFFFFF0, 0x100000010, 0x100000020]
    assert unwrap_ts([(5, 0, b""), (3, 0, b"")])[1][0] == 3  # small backstep != wrap


def test_stream_encoding():
    assert enc_stream_config(500, 0xFF, [(0x4A, 3, 2)]) == bytes(
        [0x86, 0xF4, 1, 0, 0, 0xFF, 1, 0x4A, 3, 2])
    r = bytes([0x89, 0, 1, 7, 5, 0, 0, 0]) + bytes([0x10, 0x27, 0, 0, 5, 0xAB, 0xCD])
    assert dec_stream_read(r) == (0, 5, [(10000, 5, b"\xab\xcd")])


# --- replay of the synthetic capture ----------------------------------------
def test_replay_reproduces_known_numbers():
    p = run("replay", str(FIXTURE), "--format", "json")
    assert p.returncode == 0, p.stderr
    env = json.loads(p.stdout)
    d = env["data"]
    assert env["ok"] and env["issues"] == []
    assert (d["source"], d["probe"], d["inferences"], d["dropped"]) == ("replay", None, 20, 0)
    assert d["latency_us"] == {"median": pytest.approx(10000), "p90": pytest.approx(10000)}
    r1, r2 = d["rails"]
    assert r1["addr"] == "0x4A" and r2["addr"] == "0x40"
    assert r1["avg_idle_mw"] == pytest.approx(100, rel=1e-3)
    assert r1["avg_active_mw"] == pytest.approx(300, rel=1e-3)
    assert r1["energy_per_inference_mj"] == pytest.approx(2.0, rel=1e-3)
    assert r1["gross_energy_per_inference_mj"] == pytest.approx(3.0, rel=1e-3)
    assert r2["avg_idle_mw"] == pytest.approx(80, rel=1e-3)
    assert r2["energy_per_inference_mj"] == pytest.approx(0.0, abs=1e-3)
    assert r2["gross_energy_per_inference_mj"] == pytest.approx(0.8, rel=1e-3)


def test_replay_text_and_missing_file():
    assert "20 inferences" in run("replay", str(FIXTURE)).stdout
    p = run("replay", "nope.jsonl", "--format", "json")
    env = json.loads(p.stdout)
    assert p.returncode == 1 and env["ok"] is False and env["data"] is None
    assert env["issues"][0]["severity"] == "error"


# --- analysis edge cases ----------------------------------------------------
def _flat(n=100, marker=0, w=0.1):
    return [Sample(i * 0.0005, [w], marker, False) for i in range(n)]


def codes(issues):
    return {i["code"] for i in issues}


def test_no_pulses_gives_null_not_zero():
    res, issues = analysis.analyze(_flat(), 1, 500, 0.01)
    assert res["inferences"] == 0
    assert res["latency_us"] == {"median": None, "p90": None}
    r = res["rails"][0]
    assert r["energy_per_inference_mj"] is None and r["avg_active_mw"] is None
    assert r["avg_idle_mw"] == pytest.approx(100)
    assert "no_inferences" in codes(issues)


def test_dropped_and_i2c_error_warn_and_hold():
    s = _flat(20)
    s[5] = Sample(s[5].t_s, [None], 0, False)  # i2c error
    s[10] = Sample(s[10].t_s, [0.1], 0, True)  # dropped before
    res, issues = analysis.analyze(s, 1, 500, 1.0, dropped=3)
    assert {"dropped_samples", "i2c_errors"} <= codes(issues)
    assert res["rails"][0]["avg_idle_mw"] == pytest.approx(100)  # held, not zero


def test_period_longer_than_pulse_warns():
    s = [Sample(i * 0.001, [0.1], 1 if i % 4 == 1 else 0, False) for i in range(40)]
    _, issues = analysis.analyze(s, 1, 2000, 0.0)  # period 2 ms > 1 ms pulse
    assert "period_exceeds_pulse" in codes(issues)


def test_capture_jsonl_roundtrip(tmp_path):
    header, rows, dropped = capture.read_jsonl(FIXTURE)
    out = tmp_path / "c.jsonl"
    capture.write_jsonl(out, header, rows, 7)
    assert capture.read_jsonl(out) == (header, rows, 7)
    assert cli.main(["replay", str(out), "--format", "json"]) == 0


class FakeProbe:
    def __init__(self, stream=True):
        self.log, self.stream, self.reads = [], stream, 0

    def require_stream(self):
        if not self.stream:
            raise capture.ProbeError("no stream support")
        return {"version": 2}

    def pin_names(self):
        return ["A", "TARGET_DONE"]

    def i2c_config(self, hz):
        self.log.append(("hz", hz))

    def i2c_write(self, addr, data):
        self.log.append(("w", addr, data.hex()))

    def stream_config(self, period, marker, chans):
        self.log.append(("cfg", period, marker, chans))

    def stream_start(self):
        self.log.append("start")

    def stream_stop(self):
        self.log.append("stop")

    def stream_read(self):
        self.reads += 1
        if self.reads == 1:
            return 0, 0, [(0xFFFFFFF0, 1, b"\x00\x64"), (0x10, 0, b"\x00\x64")]
        return 0, 0, []


def test_capture_sequence_unwrap_and_stop():
    mon = [parse_monitor("5V=ina236@0x4A,shunt=0.02,range=fine")]
    header, rows, _ = capture.capture(FakeProbe(), mon, "TARGET_DONE", 0.01, 0, 500, 1000000)
    assert [r[0] for r in rows] == [0xFFFFFFF0, 0x100000010]
    p = FakeProbe()
    capture.capture(p, mon, "TARGET_DONE", 0.01, 0, 500, 1000000)
    assert p.log == ["stop", ("hz", 1000000), ("w", 0x4A, "005007"),
                     ("w", 0x4A, "050800"), ("cfg", 500, 1, [(0x4A, 3, 2)]), "start", "stop"]
    with pytest.raises(capture.ProbeError):
        capture.capture(FakeProbe(stream=False), mon, None, 0.01, 0, 500, 1000000)


MON = [parse_monitor("5V=ina236@0x4A,shunt=0.02,range=fine")]


def test_capture_total_is_idle_plus_active(monkeypatch):
    t = [0.0]
    monkeypatch.setattr(capture.time, "monotonic", lambda: t[0])
    monkeypatch.setattr(capture.time, "sleep", lambda s: t.__setitem__(0, t[0] + 1))
    p = FakeProbe()
    capture.capture(p, MON, None, 5, 3, 500, 1000000)
    assert p.reads >= 8  # one read per simulated second over idle(3)+active(5)


def test_signal_stops_stream():
    class Sig(FakeProbe):
        def stream_read(self):
            signal.raise_signal(signal.SIGTERM)

    p = Sig()
    with pytest.raises(ProbeError, match="interrupted"):
        capture.capture(p, MON, None, 5, 0, 500, 1000000)
    assert p.log[-1] == "stop"
    assert signal.getsignal(signal.SIGTERM) is not capture._raise_on_signal


def test_stream_read_status_is_checked():
    class Busy(FakeProbe):
        def stream_read(self):
            return 6, 0, []

    p = Busy()
    with pytest.raises(ProbeError, match="not configured"):
        capture.capture(p, MON, None, 0.01, 0, 500, 1000000)
    assert p.log[-1] == "stop"


def test_dec_stream_read_validates_length():
    with pytest.raises(ProbeError):
        dec_stream_read(bytes([0x89, 0, 0]))
    with pytest.raises(ProbeError):  # claims 2 records, carries 1
        dec_stream_read(bytes([0x89, 0, 2, 7, 0, 0, 0, 0]) + bytes(7))
    with pytest.raises(ProbeError):  # record size disagrees with the config
        dec_stream_read(bytes([0x89, 0, 1, 7, 0, 0, 0, 0]) + bytes(7), recsize=9)


def test_missing_active_samples_is_null_not_zero():
    s2 = [Sample(i * 0.0005, [None], 1 if 4 <= i < 8 else 0, False)
          for i in range(20)]
    res, issues = analysis.analyze(s2, 1, 500, 1.0, names=["5V"])
    assert res["inferences"] == 1
    r = res["rails"][0]
    assert r["avg_active_mw"] is None and r["energy_per_inference_mj"] is None
    assert r["gross_energy_per_inference_mj"] is None
    assert "no_active_samples" in codes(issues)


def test_zero_idle_warns():
    _, issues = analysis.analyze(_flat(), 1, 500, 0.0)
    assert "no_idle_window" in codes(issues)


def test_pin_names_keep_placeholder():
    from alp_power.transport import Probe
    p = Probe.__new__(Probe)
    replies = {0x80: bytes([0x80, 2, 7, 3, 59]), (0x85, 0): bytes([0x85, 0, 0, 1]) + b"A",
               (0x85, 1): bytes([0x85, 1, 0, 0]), (0x85, 2): bytes([0x85, 0, 0, 1]) + b"C"}
    p.xfer = lambda req, minlen=2: replies[req[0] if req[0] == 0x80 else (req[0], req[1])]
    assert p.pin_names() == ["A", None, "C"]


def test_json_usage_error_and_internal_error(monkeypatch, capsys):
    with pytest.raises(SystemExit) as ei:
        cli.main(["measure", "--format", "json", "--bogus"])
    assert ei.value.code == 1
    env = json.loads(capsys.readouterr().out)
    assert env["ok"] is False and env["issues"][0]["code"] == "usage_error"
    monkeypatch.setattr(cli, "_measure", lambda a: 1 / 0)
    assert cli.main(["measure", "--monitor", "x=ina236@0x4A,shunt=1", "--format", "json"]) == 1
    env = json.loads(capsys.readouterr().out)
    assert env["issues"][0]["code"] == "internal_error"
    assert "ZeroDivisionError" in env["issues"][0]["message"]


def test_write_failure_keeps_result(monkeypatch, tmp_path, capsys):
    header, rows, dropped = capture.read_jsonl(FIXTURE)
    monkeypatch.setattr(cli.cap, "capture", lambda *a: (header, rows, dropped))
    monkeypatch.setattr(cli, "Probe", lambda: None)
    out = str(tmp_path / "no" / "such" / "c.jsonl")
    argv = ["measure", "--monitor", "5V=ina236@0x4A,shunt=0.02", "--out", out, "--format", "json"]
    assert cli.main(argv) == 0
    env = json.loads(capsys.readouterr().out)
    assert env["ok"] and env["data"]["inferences"] == 20
    assert "capture_write_failed" in codes(env["issues"])


def _pulse_samples(spec):
    """spec: marker string per sample, 'd' after a char = dropped before it."""
    return [Sample(i * 0.0005, [0.1 + 0.1 * m], m, False) for i, m in enumerate(spec)]


def test_partial_pulses_excluded():
    s = _pulse_samples([1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1])
    res, issues = analysis.analyze(s, 1, 500, 0.0)
    assert res["inferences"] == 1
    assert {"partial_pulse_start", "partial_pulse_end"} <= codes(issues)
    assert res["rails"][0]["avg_active_mw"] == pytest.approx(200)
    assert res["latency_us"]["median"] == pytest.approx(1000)


def test_pulse_spanning_drop_excluded():
    s = _pulse_samples([0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0])
    s[3] = s[3]._replace(dropped=True)
    res, issues = analysis.analyze(s, 1, 500, 0.0)
    assert res["inferences"] == 1
    assert "pulse_spans_drop" in codes(issues)
