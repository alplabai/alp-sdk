# scripts/alp_model/measure.py
"""Measure: a host reference run + A/B compare + estimate-vs-measured feedback.

The host run (`backend="cpu-host"`) is a FUNCTIONAL + host-latency reference,
NOT the target SoM's performance. On-device measurement (target latency, peak
arena SRAM, per-rail energy via the on-board monitor IC) is the HW-gated
follow-on that fills the SAME RunResult schema with real numbers. `power_mj`
and `peak_sram_kib` stay None on the host.

Energy sample contract (Phase A — pure math only, no I2C/firmware here):
a `PowerSample` is `(t_seconds, power_watts)`. `power_watts` is read straight
from the on-board INA236's Power register (3h) — the INA236 computes P = V*I
in hardware (optionally hardware-averaged); there is no software V*I multiply
in this contract and none should be added. The INA236 has no on-chip
energy/charge accumulator (only an averaging accumulator over its Power/
Current/Bus-Voltage conversions), which is exactly why `integrate_energy`
does the energy integration host-side, from repeated Power-register reads."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from statistics import median
from time import perf_counter
from typing import Sequence

import numpy as np


class MeasureError(Exception):
    """The model could not be loaded or run for measurement."""


# (t_seconds, power_watts) — power_watts is an INA236 Power-register (3h) read.
PowerSample = tuple[float, float]


def integrate_energy(samples: Sequence[PowerSample]) -> float:
    """Trapezoidal time-integration of power samples into millijoules:
    E = Σ 0.5*(P_i + P_i+1)*Δt over consecutive samples (Δt in seconds,
    P in watts -> joules, *1000 -> mJ). Samples must be in ascending t order
    (the INA236 has no on-chip energy accumulator, so this is the whole
    point of taking repeated Power-register samples host-side).
    <2 samples -> 0.0 (no interval to integrate; not an error — a single
    reading carries no energy information, so returning 0.0 lets a caller
    that hasn't collected a real window yet compose without special-casing)."""
    if len(samples) < 2:
        return 0.0
    energy_j = 0.0
    for (t0, p0), (t1, p1) in zip(samples, samples[1:]):
        energy_j += 0.5 * (p0 + p1) * (t1 - t0)
    return energy_j * 1000.0


def windowed_delta(active_samples: Sequence[PowerSample],
                    idle_samples: Sequence[PowerSample], n_inferences: int) -> float:
    """Idle-baseline-subtracted energy per inference, in mJ: the delta between
    an active (inferring) window and an idle window, divided by how many
    inferences ran in the active window.

    Precondition: active_samples and idle_samples MUST cover equal wall-clock
    duration (same integration window length). This is a subtraction of two
    integrals, not a rate comparison -- unequal durations bias the baseline
    (e.g. a longer idle window subtracts more idle energy than the active
    window actually contains). The caller/firmware is responsible for taking
    symmetric windows; this function does not (and should not) assert it,
    since real hardware timing has jitter that would make a hard equality
    check false-trip."""
    if n_inferences < 1:
        raise ValueError(f"n_inferences must be >= 1, got {n_inferences}")
    return (integrate_energy(active_samples) - integrate_energy(idle_samples)) / n_inferences


@dataclass(frozen=True)
class EnergyMeasurement:
    """A real bench-measured energy result — never fabricate one; `run_host()`
    leaves `RunResult.energy` as None instead. `source`/`scope` default to (and
    are validated as) the honest labels: this is a board-level carrier-rail
    delta, NEVER an isolated NPU/U85/U55/M55 silicon energy figure."""
    value_mj_per_inference: float
    rails: list[str]
    n_inferences: int
    window_ms: float
    sample_count: int
    spread_mj: float | None = None      # std/spread across M repeated windows;
                                         # None for a single window
    source: str = "measured"
    scope: str = "carrier-rail-delta"

    def __post_init__(self) -> None:
        if self.source != "measured" or self.scope != "carrier-rail-delta":
            raise ValueError(
                "EnergyMeasurement is board-level-delta only: source must be "
                "'measured' and scope 'carrier-rail-delta' — never label this "
                "NPU/U85/U55/M55 silicon energy.")


@dataclass(frozen=True)
class RunResult:
    backend: str                 # "cpu-host" (reference) | on-device backend later
    latency_ms: float            # median wall-clock per inference
    output_argmax: int | None
    peak_sram_kib: int | None    # None on host — on-device only
    power_mj: float | None       # None on host — on-board monitor read (HW-gated)
    runs: int
    energy: EnergyMeasurement | None = None  # None until a real bench run populates it


def default_input(onnx_path: Path, *, seed: int = 0) -> np.ndarray:
    """Deterministic random sample matching the model's first input shape.
    Dynamic dims collapse to 1 (intent: a dynamic BATCH dim; a symbolic H/W
    would also become 1). Wrapped in MeasureError so a bad model fails clean."""
    import onnxruntime as ort
    try:
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        shape = [(1 if not isinstance(d, int) else d) for d in sess.get_inputs()[0].shape]
    except Exception as exc:
        raise MeasureError(f"could not load model {Path(onnx_path).name}: {exc}") from exc
    return np.random.default_rng(seed).standard_normal(shape).astype(np.float32)


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
    latency_ratio: float | None  # b.latency_ms / a.latency_ms; None if a.latency_ms == 0
                                  # (avoids emitting non-finite JSON `Infinity`)
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
    ratio = (b.latency_ms / a.latency_ms) if a.latency_ms else None
    delta = (size_b - size_a) if (size_a is not None and size_b is not None) else None
    return ABComparison(faster=faster,
                        latency_ratio=round(ratio, 4) if ratio is not None else None,
                        a_latency_ms=a.latency_ms, b_latency_ms=b.latency_ms,
                        size_delta_bytes=delta)


def estimate_vs_measured(est_latency_ms: float | None, measured_latency_ms: float) -> dict:
    """The self-improving loop: how far #1's static estimate was from reality.
    (Later) calibrates the tier-1 estimator. Decoupled — takes two numbers."""
    ratio = (measured_latency_ms / est_latency_ms) if est_latency_ms else None
    return {"est_latency_ms": est_latency_ms, "measured_latency_ms": measured_latency_ms,
            "ratio": round(ratio, 4) if ratio is not None else None}
