# SPDX-License-Identifier: Apache-2.0
"""Pure analysis: samples -> per-rail idle/active power and energy per inference.

Each sample holds its value until the next one (the probe samples marker and
monitors in the same instant), so energy = sum(P_i * dt_i).  The marker's
resolution is one sample period: latency and pulse boundaries are quantised
to it.  The method mirrors the windowed idle-baseline subtraction in
docs/measuring-inference-energy.md.
"""
import statistics
from collections import namedtuple

# t_s: seconds from the first sample; watts: list per rail (None = I2C error);
# marker: 0/1; dropped: records were lost just before this one.
Sample = namedtuple("Sample", "t_s watts marker dropped")


def _issue(code, severity, message):
    return {"code": code, "severity": severity, "message": message}


def _p90(sorted_vals):
    return sorted_vals[max(0, -(-9 * len(sorted_vals) // 10) - 1)]  # nearest rank


def analyze(samples, n_rails, period_us, idle_s, dropped=0):
    """-> (result dict {inferences, latency_us, duration_s, rails:[...]}, issues)."""
    issues, period = [], period_us / 1e6
    if len(samples) < 2:
        raise ValueError("capture has fewer than 2 samples")
    # dt_i: until the next sample; nominal period across a gap or at the end.
    dts = [period if samples[i + 1].dropped else samples[i + 1].t_s - samples[i].t_s
           for i in range(len(samples) - 1)] + [period]
    # Hold the last good reading across an I2C error (all channels are zeroed).
    held, good, errs = [], [None] * n_rails, 0
    for s in samples:
        if any(w is None for w in s.watts):
            errs += 1
        good = [g if w is None else w for w, g in zip(s.watts, good)]
        held.append(list(good))

    rising = sum(1 for a, b in zip(samples, samples[1:]) if not a.marker and b.marker)
    pulses, start = [], None
    for s in samples:
        if s.marker and start is None:
            start = s.t_s
        elif not s.marker and start is not None:
            pulses.append((s.t_s - start) * 1e6)
            start = None
    pulses.sort()
    latency = {"median": statistics.median(pulses) if pulses else None,
               "p90": _p90(pulses) if pulses else None}

    t_high = sum(d for s, d in zip(samples, dts) if s.marker)
    idle_idx = [i for i, s in enumerate(samples) if not s.marker and s.t_s < idle_s]
    if not idle_idx:
        idle_idx = [i for i, s in enumerate(samples) if not s.marker]
        if idle_idx:
            issues.append(_issue("idle_fallback", "info", "no marker-low samples in the idle "
                                 "window; baseline is the mean of all marker-low samples"))

    rails = []
    for r in range(n_rails):
        usable = [i for i in range(len(samples)) if held[i][r] is not None]
        idle = [held[i][r] for i in idle_idx if held[i][r] is not None]
        e_high = sum(held[i][r] * dts[i] for i in usable if samples[i].marker)
        base = sum(idle) / len(idle) if idle else None
        rails.append({
            "base_w": base,
            "avg_idle_mw": None if base is None else base * 1e3,
            "avg_active_mw": e_high / t_high * 1e3 if t_high > 0 and e_high is not None else None,
            "gross_energy_per_inference_mj": e_high * 1e3 / rising if rising else None,
            "energy_per_inference_mj": ((e_high - base * t_high) * 1e3 / rising
                                       if rising and base is not None else None),
            "samples": len(usable)})
    for r in rails:
        r.pop("base_w")

    if not rising:
        issues.append(_issue("no_inferences", "warning", "no marker rising edges: "
                             "energy per inference is unavailable"))
    elif pulses and period > statistics.median(pulses) / 1e6:
        issues.append(_issue("period_exceeds_pulse", "warning",
                             "sample period is longer than the median marker-high time: "
                             "energy per inference is a statistical estimate over many pulses"))
    if dropped:
        issues.append(_issue("dropped_samples", "warning", f"{dropped} samples dropped; "
                             "raise --period-us or reduce the monitors"))
    if errs:
        issues.append(_issue("i2c_errors", "warning", f"{errs} samples had an I2C error; "
                             "their power was held at the previous reading"))
    return {"inferences": rising, "latency_us": latency, "rails": rails,
            "duration_s": samples[-1].t_s - samples[0].t_s}, issues
