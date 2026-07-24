# scripts/alp_model/measure.py
"""Measure: a host reference run + A/B compare + estimate-vs-measured feedback.

The host run (`backend="cpu-host"`) is a FUNCTIONAL + host-latency reference,
NOT the target SoM's performance. On-device measurement (target latency, peak
arena SRAM, per-rail energy via the on-board monitor IC) is the HW-gated
follow-on that fills the SAME RunResult schema with real numbers. `power_mj`
and `peak_sram_kib` stay None on the host."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from statistics import median
from time import perf_counter

import numpy as np


class MeasureError(Exception):
    """The model could not be loaded or run for measurement."""


@dataclass(frozen=True)
class RunResult:
    backend: str                 # "cpu-host" (reference) | on-device backend later
    latency_ms: float            # median wall-clock per inference
    output_argmax: int | None
    peak_sram_kib: int | None    # None on host — on-device only
    power_mj: float | None       # None on host — on-board monitor read (HW-gated)
    runs: int


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
    latency_ratio: float         # b.latency_ms / a.latency_ms
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
    ratio = (b.latency_ms / a.latency_ms) if a.latency_ms else float("inf")
    delta = (size_b - size_a) if (size_a is not None and size_b is not None) else None
    return ABComparison(faster=faster, latency_ratio=round(ratio, 4),
                        a_latency_ms=a.latency_ms, b_latency_ms=b.latency_ms,
                        size_delta_bytes=delta)


def estimate_vs_measured(est_latency_ms: float | None, measured_latency_ms: float) -> dict:
    """The self-improving loop: how far #1's static estimate was from reality.
    (Later) calibrates the tier-1 estimator. Decoupled — takes two numbers."""
    ratio = (measured_latency_ms / est_latency_ms) if est_latency_ms else None
    return {"est_latency_ms": est_latency_ms, "measured_latency_ms": measured_latency_ms,
            "ratio": round(ratio, 4) if ratio is not None else None}
