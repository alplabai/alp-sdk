# SPDX-License-Identifier: Apache-2.0
"""alp_power CLI: measure | replay | probe-info.  JSON envelope is consumed by
the VS Code extension: {"ok", "data", "issues"}."""
import argparse
import json
import sys

from . import analysis, capture as cap
from .monitors import parse_monitor
from .transport import Probe, ProbeError


def _issue(code, message, severity="error"):
    return {"code": code, "severity": severity, "message": message}


def _emit(env, fmt, text):
    print(json.dumps(env, indent=2) if fmt == "json" else text(env))
    return 0 if env["ok"] else 1


def _fail(fmt, code, message):
    return _emit({"ok": False, "data": None, "issues": [_issue(code, message)]}, fmt,
                 lambda e: f"error: {message}")


def _note(header):
    return (f"Energy per inference = (integral of power over marker-high time - idle baseline "
            f"x marker-high time) / complete marker pulses; idle baseline = mean power while "
            f"the marker is low in the first {header['idle_s']} s (the idle window, captured "
            f"before the active window). Times are quantised to the {header['period_us']} us "
            f"sample period. Not validated on hardware.")


def _r(v):
    return None if v is None else round(v, 6)


def _result(source, header, rows, dropped):
    samples = cap.to_samples(header, rows)
    res, issues = analysis.analyze(samples, len(header["monitors"]), header["period_us"],
                                   header["idle_s"], dropped,
                                   [h["name"] for h in header["monitors"]])
    for r, h in zip(res["rails"], header["monitors"]):
        r.update(name=h["name"], part=h["part"], addr=h["addr"])
    data = {"source": source,
            "probe": {"protocol": header["protocol"]} if source == "probe" else None,
            "period_us": header["period_us"], "duration_s": _r(res["duration_s"]),
            "inferences": res["inferences"], "latency_us": {k: _r(v) for k, v in res["latency_us"].items()},
            "dropped": dropped,
            "rails": [{**{k: r[k] for k in ("name", "part", "addr", "samples")},
                       **{k: _r(r[k]) for k in ("avg_idle_mw", "avg_active_mw",
                                                "energy_per_inference_mj",
                                                "gross_energy_per_inference_mj")}}
                      for r in res["rails"]],
            "note": _note(header)}
    return {"ok": True, "data": data, "issues": issues}


def _num(v, unit):
    return "n/a" if v is None else f"{v:.3f} {unit}"


def _text(env):
    if not env["ok"]:
        return "error: " + "; ".join(i["message"] for i in env["issues"])
    d = env["data"]
    lat = d["latency_us"]
    lines = [f"{d['source']}: {d['inferences']} inferences over {d['duration_s']:.3f} s, "
             f"period {d['period_us']} us, dropped {d['dropped']}",
             f"latency us: median {lat['median']} p90 {lat['p90']}"]
    for r in d["rails"]:
        lines.append(f"  {r['name']} ({r['part']}@{r['addr']}): idle {_num(r['avg_idle_mw'], 'mW')}, "
                     f"active {_num(r['avg_active_mw'], 'mW')}, "
                     f"{_num(r['energy_per_inference_mj'], 'mJ')}/inference "
                     f"(gross {_num(r['gross_energy_per_inference_mj'], 'mJ')})")
    lines += [f"{i['severity']}: {i['message']}" for i in env["issues"]]
    return "\n".join(lines)


def _measure(a):
    monitors = [parse_monitor(s) for s in a.monitor]
    names = [n for n, _ in monitors]
    if len(set(names)) != len(names):
        raise ValueError("duplicate monitor name")
    if not 200 <= a.period_us <= 10_000_000:
        raise ValueError("--period-us must be 200..10000000")
    if not 1 <= len(monitors) <= 8:
        raise ValueError("1..8 monitors supported")
    header, rows, dropped = cap.capture(Probe(), monitors, a.marker, a.seconds,
                                        a.idle_seconds, a.period_us, a.i2c_hz)
    env = _result("probe", header, rows, dropped)
    if a.out:
        try:
            cap.write_jsonl(a.out, header, rows, dropped)
        except OSError as e:
            env["issues"].append(_issue("capture_write_failed", f"could not write {a.out}: {e}",
                                        "warning"))
    return env


def _probe_info(a):
    p = Probe()
    info = p.info()
    pins = p.pin_names() if info["gpio"] else []
    return {"ok": True, "data": {"protocol": info["version"], "features": {
        "i2c": info["i2c"], "gpio": info["gpio"], "stream": info["stream"]},
        "i2c_max": info["i2c_max"], "pins": pins}, "issues": []}


class _JsonArgs(argparse.ArgumentParser):
    """With --format json, usage errors are a JSON envelope (exit 1), not stderr text."""
    json_mode = False

    def error(self, message):
        if not _JsonArgs.json_mode:
            super().error(message)
        _fail("json", "usage_error", message)
        sys.exit(1)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    json_mode = any(x == "--format=json" or (x == "json" and argv[i - 1:i] == ["--format"])
                    for i, x in enumerate(argv))
    _JsonArgs.json_mode = json_mode
    ap = _JsonArgs(prog="alp_power", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure", help="capture from the probe and analyse")
    m.add_argument("--monitor", action="append", required=True,
                   metavar="NAME=ina236@0x4A,shunt=0.02[,range=fine|wide]")
    m.add_argument("--marker", metavar="PIN_NAME")
    m.add_argument("--seconds", type=float, default=10,
                   help="ACTIVE window (target inferring), captured after the idle window")
    m.add_argument("--idle-seconds", type=float, default=3,
                   help="extra idle baseline window captured FIRST (target idle); "
                        "total capture = idle + seconds")
    m.add_argument("--period-us", type=int, default=500)
    m.add_argument("--i2c-hz", type=int, default=1_000_000)
    m.add_argument("--out", metavar="capture.jsonl")
    r = sub.add_parser("replay", help="analyse a saved capture (no hardware)")
    r.add_argument("capture")
    sub.add_parser("probe-info")
    for p in (m, r, sub.choices["probe-info"]):
        p.add_argument("--format", choices=["json", "text"], default="text")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "replay":
            env = _result("replay", *cap.read_jsonl(a.capture))
        else:
            env = _measure(a) if a.cmd == "measure" else _probe_info(a)
    except ProbeError as e:
        return _fail(a.format, "probe_error", str(e))
    except (OSError, ValueError, KeyError) as e:
        return _fail(a.format, "invalid_input", str(e))
    except Exception as e:  # last resort: consumers always get an envelope
        return _fail(a.format, "internal_error", f"{type(e).__name__}: {e}")
    return _emit(env, a.format, _text if a.cmd != "probe-info" else
                 lambda e: json.dumps(e["data"], indent=2))


if __name__ == "__main__":
    sys.exit(main())
