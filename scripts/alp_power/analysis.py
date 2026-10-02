# SPDX-License-Identifier: Apache-2.0
"""Pure analysis: samples -> per-rail idle/active power and energy per inference.

Each sample holds its value until the next one (the probe samples marker and
monitors in the same instant), so energy = sum(P_i * dt_i).  The marker's
resolution is one sample period: latency and pulse boundaries are quantised
to it.  Only complete marker pulses are counted: a pulse already high at the
first sample, still high at the last, or spanning a dropped-sample gap is
excluded from the count, the energy and the latency together, so the numbers
stay consistent.  The method mirrors the windowed idle-baseline subtraction in
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


def _pulses(samples, issues):
    """-> list of (first, end) index ranges of complete marker-high runs."""
    n, runs, i = len(samples), [], 0
    while i < n:
        if not samples[i].marker:
            i += 1
            continue
        a = i
        while i < n and samples[i].marker:
            i += 1
        runs.append((a, i))
    good, spanned = [], 0
    for a, b in runs:
        if a == 0:
            issues.append(_issue("partial_pulse_start", "info", "marker was already high at "
                                 "the first sample; that pulse is excluded"))
        elif b == n:
            issues.append(_issue("partial_pulse_end", "info", "marker still high at the last "
                                 "sample; that pulse is excluded"))
        elif any(samples[k].dropped for k in range(a + 1, b + 1)):
            spanned += 1
        else:
            good.append((a, b))
    if spanned:
        issues.append(_issue("pulse_spans_drop", "warning", f"{spanned} marker pulse(s) span "
                             "dropped samples and are excluded from the numbers"))
    return good


def analyze(samples, n_rails, period_us, idle_s, dropped=0, names=None):
    """-> (result dict {inferences, latency_us, duration_s, rails:[...]}, issues)."""
    issues, period = [], period_us / 1e6
    names = names or [f"rail {r}" for r in range(n_rails)]
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

    pulses = _pulses(samples, issues)
    count = len(pulses)
    latency_list = sorted((samples[b].t_s - samples[a].t_s) * 1e6 for a, b in pulses)
    latency = {"median": statistics.median(latency_list) if latency_list else None,
               "p90": _p90(latency_list) if latency_list else None}
    high_idx = [i for a, b in pulses for i in range(a, b)]

    idle_idx = [i for i, s in enumerate(samples) if not s.marker and s.t_s < idle_s]
    if not idle_idx:
        idle_idx = [i for i, s in enumerate(samples) if not s.marker]
        if idle_idx:
            issues.append(_issue("idle_fallback", "info", "no marker-low samples in the idle "
                                 "window; baseline is the mean of all marker-low samples"))
    if idle_s <= 0:
        issues.append(_issue("no_idle_window", "warning", "--idle-seconds is 0: the baseline "
                             "is the mean of all marker-low samples, not a dedicated idle window"))

    rails = []
    for r in range(n_rails):
        usable = [i for i in high_idx if held[i][r] is not None]
        t_high = sum(dts[i] for i in usable)
        e_high = sum(held[i][r] * dts[i] for i in usable)
        idle = [held[i][r] for i in idle_idx if held[i][r] is not None]
        base = sum(idle) / len(idle) if idle else None
        has = bool(count and usable and t_high > 0)
        if count and not has:
            issues.append(_issue("no_active_samples", "warning", f"{names[r]}: no usable "
                                 "samples inside marker-high pulses (I2C errors?); active "
                                 "power and energy are unavailable"))
        rails.append({
            "avg_idle_mw": None if base is None else base * 1e3,
            "avg_active_mw": e_high / t_high * 1e3 if has else None,
            "gross_energy_per_inference_mj": e_high * 1e3 / count if has else None,
            "energy_per_inference_mj": ((e_high - base * t_high) * 1e3 / count
                                        if has and base is not None else None),
            "samples": sum(1 for i in range(len(samples)) if held[i][r] is not None)})

    if not count:
        issues.append(_issue("no_inferences", "warning", "no complete marker pulses: "
                             "energy per inference is unavailable"))
    elif period > statistics.median(latency_list) / 1e6:
        issues.append(_issue("period_exceeds_pulse", "warning",
                             "sample period is longer than the median marker-high time: "
                             "energy per inference is a statistical estimate over many pulses"))
    if dropped:
        issues.append(_issue("dropped_samples", "warning", f"{dropped} samples dropped; "
                             "raise --period-us or reduce the monitors"))
    if errs:
        issues.append(_issue("i2c_errors", "warning", f"{errs} samples had an I2C error; "
                             "their power was held at the previous reading"))
    return {"inferences": count, "latency_us": latency, "rails": rails,
            "duration_s": samples[-1].t_s - samples[0].t_s}, issues
