# SPDX-License-Identifier: Apache-2.0
"""Capture records from the probe, store/load them as JSONL, decode to samples.

JSONL layout: a header line {"type":"header",...} carrying the monitor config,
one {"t_us":int,"flags":int,"data":hex} line per record (u32 timestamps already
unwrapped), and a trailer {"type":"end","dropped":int}.
"""
import json
import signal
import time

from .analysis import Sample
from .monitors import monitor_from_header, monitor_header
from .transport import NO_MARKER, ProbeError, status_text, unwrap_ts

FLAG_MARKER, FLAG_I2C_ERR, FLAG_DROPPED = 1, 2, 4


def _read(probe):
    st, dropped, recs = probe.stream_read()
    if st:
        raise ProbeError(f"stream read failed: {status_text(st)}")
    return dropped, recs


def _raise_on_signal(signum, _frame):
    raise ProbeError(f"interrupted by signal {signum}")


def capture(probe, monitors, marker, seconds, idle_s, period_us, i2c_hz):
    """monitors: [(name, monitor)].  Captures idle_s (baseline) then `seconds`
    (active).  -> (header, [(t_us, flags, data)], dropped)."""
    info = probe.require_stream()  # before the handlers/STOP: old firmware may not know STOP
    old = {}
    for sig in (signal.SIGINT, signal.SIGTERM):  # so `finally` can STOP the probe
        try:
            old[sig] = signal.signal(sig, _raise_on_signal)
        except ValueError:  # not the main thread
            pass
    rows, dropped = [], 0
    try:
        probe.stream_stop()  # idempotent: clears a stream a killed earlier run left running
        marker_idx = NO_MARKER
        if marker:
            names = probe.pin_names()
            if marker not in names:
                raise ProbeError(f"probe has no pin named {marker!r} (pins: "
                                 f"{', '.join(n for n in names if n)})")
            marker_idx = names.index(marker)
        probe.i2c_config(i2c_hz)
        chans = []
        for _, m in monitors:
            for payload in m.configure():
                probe.i2c_write(m.addr, payload)
            chans += m.stream_channels()
        probe.stream_config(period_us, marker_idx, chans)
        probe.stream_start()
        end = time.monotonic() + idle_s + seconds
        while time.monotonic() < end:
            dropped, recs = _read(probe)
            rows += recs
            if not recs:
                time.sleep(0.001)
    finally:  # never leave the probe sampling (and I2C busy) after an error or signal
        try:
            probe.stream_stop()
        finally:
            for sig, h in old.items():
                signal.signal(sig, h)
    while True:  # drain what the sampler left in the ring
        dropped, recs = _read(probe)
        if not recs:
            break
        rows += recs
    header = {"type": "header", "version": 1, "protocol": info["version"],
              "period_us": period_us, "idle_s": idle_s, "seconds": seconds, "i2c_hz": i2c_hz,
              "marker": marker, "monitors": [monitor_header(n, m) for n, m in monitors]}
    return header, unwrap_ts(rows), dropped


def write_jsonl(path, header, rows, dropped):
    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps(header) + "\n")
        for t, flags, data in rows:
            f.write(json.dumps({"t_us": t, "flags": flags, "data": data.hex()}) + "\n")
        f.write(json.dumps({"type": "end", "dropped": dropped}) + "\n")


def read_jsonl(path):
    """-> (header, rows, dropped); dropped falls back to counting flagged records."""
    header, rows, dropped = None, [], None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if not line.strip():
                continue
            d = json.loads(line)
            if d.get("type") == "header":
                header = d
            elif d.get("type") == "end":
                dropped = d["dropped"]
            elif "t_us" in d:
                rows.append((d["t_us"], d["flags"], bytes.fromhex(d["data"])))
            else:
                raise ValueError(f"line {n}: unrecognised record")
    if header is None:
        raise ValueError("no header line (is this an alp_power capture?)")
    if dropped is None:
        dropped = sum(1 for _, f, _ in rows if f & FLAG_DROPPED)
    return header, rows, dropped


def to_samples(header, rows):
    """Decode records to analysis Samples (power only; per rail in monitor order)."""
    mons = [monitor_from_header(h)[1] for h in header["monitors"]]
    sizes = [2 * len(m.STREAM_REGS) for m in mons]
    t0 = rows[0][0] if rows else 0
    out = []
    for t, flags, data in rows:
        if flags & FLAG_I2C_ERR:
            watts = [None] * len(mons)
        else:
            watts, o = [], 0
            for m, n in zip(mons, sizes):
                watts.append(m.decode(data[o:o + n])["watts"])
                o += n
        out.append(Sample((t - t0) / 1e6, watts, flags & FLAG_MARKER,
                          bool(flags & FLAG_DROPPED)))
    return out
