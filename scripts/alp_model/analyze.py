# scripts/alp_model/analyze.py
"""Static pre-flight fit/perf analyzer (fidelity tier 1).

Offline, no NPU toolchain: parse a TFLite graph, and for each backend a SoM
exposes, estimate op-coverage, peak arena SRAM, and rough latency -> a fit
verdict. Every number is a conservative estimate labelled `source: static`;
the tier-2 bench data (a follow-on) will calibrate these toward reality.

Correctness rule: NEVER over-promise 'fits'. Round SRAM up, treat unknown ops
as CPU-fallback, assume <=50% NPU utilisation for latency."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from .targets import _npu_backend, resolve_targets
from .tensorio import OpDesc, extract_ops

# Assume 50% NPU utilisation -> latency is pessimistic (higher). A naive roofline;
# tier-2 bench measurement replaces this per-NPU factor with a fitted model.
_UTIL = 0.5


class UnsupportedModelError(Exception):
    """The model can't be statically analysed (non-TFLite, or unparseable) --
    raised instead of emitting a fabricated verdict."""


@dataclass(frozen=True)
class BackendResult:
    backend: str
    verdict: str                       # fits | cpu-fallback | no-fit
    est_sram_kib: int
    budget_sram_kib: int | None        # None = unknown/unbounded (arena not authored)
    est_latency_ms: float | None       # None = no throughput data (e.g. cpu backend)
    op_coverage_pct: float
    unsupported_ops: list[str]
    source: str = "static"


@dataclass(frozen=True)
class AnalysisResult:
    model: str
    sku: str
    backends: list[BackendResult] = field(default_factory=list)
    suggestion: str | None = None


# --- pure estimators ------------------------------------------------------

def _op_macs(od: OpDesc) -> int:
    outs = od.outputs
    if not outs:
        return 0
    out = outs[0].shape
    name = od.op
    if name == "FULLY_CONNECTED":
        # weights = inputs[1] [units, in_features]; batch = product of out[:-1]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 2:
            units, in_f = od.inputs[1].shape
            batch = 1
            for d in out[:-1]:
                batch *= d
            return batch * units * in_f
        return 0
    if name in ("CONV_2D", "TRANSPOSE_CONV"):
        # weights [c_out,kh,kw,c_in]; out [n,oh,ow,c_out]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 4 and len(out) == 4:
            c_out, kh, kw, c_in = od.inputs[1].shape
            _, oh, ow, _ = out
            return oh * ow * c_out * kh * kw * c_in
        return 0
    if name == "DEPTHWISE_CONV_2D":
        # weights [1,kh,kw,c]; out [n,oh,ow,c]
        if len(od.inputs) >= 2 and len(od.inputs[1].shape) == 4 and len(out) == 4:
            _, kh, kw, c = od.inputs[1].shape
            _, oh, ow, _ = out
            return oh * ow * c * kh * kw
        return 0
    return 0                            # elementwise/unknown -> negligible MACs


def estimate_macs(ops: list[OpDesc]) -> int:
    return sum(_op_macs(od) for od in ops)


def estimate_peak_sram_kib(ops: list[OpDesc]) -> int:
    """Conservative upper bound: sum every activation-tensor occurrence (a tensor
    counted once as a producer's output and again as a consumer's input -> a
    deliberate over-count so a static check never falsely reports 'fits').
    Constants (weights/bias) live in flash and are excluded. Rounds up to KiB.
    ponytail: sum-of-activations upper bound; swap for a true liveness high-water
    once tier-2 bench SRAM calibrates it."""
    total = sum(t.nbytes for od in ops for t in (*od.inputs, *od.outputs)
                if not t.is_const)
    return (total + 1023) // 1024


def npu_mac_per_s(npu: dict) -> float | None:
    mpc, fmhz = npu.get("mac_per_cycle"), npu.get("freq_mhz")
    if mpc and fmhz:
        return mpc * fmhz * 1e6
    if npu.get("tops"):
        return npu["tops"] * 1e12 / 2       # 1 MAC = 2 ops
    if npu.get("gops"):
        return npu["gops"] * 1e9 / 2
    return None


def estimate_latency_ms(macs: int, npu: dict) -> float | None:
    rate = npu_mac_per_s(npu)
    if not rate:
        return None
    return macs / (rate * _UTIL) * 1e3


def op_coverage(ops: list[OpDesc], supported: set[str]) -> tuple[float, list[str]]:
    if not ops:
        return 100.0, []
    covered = sum(1 for od in ops if od.op in supported)
    unsupported = sorted({od.op for od in ops if od.op not in supported})
    return round(covered / len(ops) * 100, 1), unsupported


# --- metadata readers -----------------------------------------------------

def _load_op_support(backend: str, metadata_root: Path) -> set[str]:
    p = metadata_root / "npu_ops" / f"{backend}.json"
    if not p.is_file():
        return set()                        # no data -> everything unsupported (safe)
    return set(json.loads(p.read_text("utf-8")).get("supported_ops", []))


def _load_npu(spec, metadata_root: Path) -> tuple[dict | None, int]:
    """Re-read the SoC spec named by the resolved target's silicon_ref, and return
    (matching npu dict, arena KiB). Prefers the npu whose accel-config matches the
    target (a SoC may expose two Ethos-U configs)."""
    vendor, family, variant = spec.silicon_ref.split(":")
    soc = json.loads(
        (metadata_root / "socs" / vendor / family / f"{variant}.json").read_text("utf-8"))
    arena = int(soc.get("inference_arena_sram_kib", 0))
    match = None
    for npu in soc.get("npus", []):
        if _npu_backend(npu.get("type", ""), npu.get("subtype", "")) != spec.backend:
            continue
        match = match or npu
        if spec.accel_config and \
                f"{npu.get('type', '')}-{npu.get('mac_per_cycle')}" == spec.accel_config:
            return npu, arena               # exact accel-config match wins
    return match, arena


def _suggest(backends: list[BackendResult]) -> str | None:
    for b in backends:
        if b.backend == "cpu":
            continue
        if b.verdict == "no-fit":
            return (f"{b.backend}: arena overflow (~{b.est_sram_kib} KiB > "
                    f"{b.budget_sram_kib} KiB budget) -- quantize/prune, or move to a "
                    f"larger in-family SoM.")
        if b.verdict == "cpu-fallback" and b.unsupported_ops:
            return (f"{b.backend}: {len(b.unsupported_ops)} op(s) fall back to CPU "
                    f"({', '.join(b.unsupported_ops)}) -- substitute NPU-friendly ops "
                    f"or re-quantize.")
    return None


def analyze_model(model_path: Path, sku: str, *, metadata_root: Path) -> AnalysisResult:
    ops = extract_ops(model_path)
    if not ops:
        raise UnsupportedModelError(
            f"static check supports .tflite models in this release; cannot analyse "
            f"{model_path.name} (ONNX static analysis is a follow-on)")

    macs = estimate_macs(ops)
    sram_kib = estimate_peak_sram_kib(ops)
    backends: list[BackendResult] = []

    for spec in resolve_targets(sku, metadata_root=metadata_root):
        if spec.backend == "cpu":
            backends.append(BackendResult(
                backend="cpu", verdict="fits", est_sram_kib=sram_kib,
                budget_sram_kib=None, est_latency_ms=None,
                op_coverage_pct=100.0, unsupported_ops=[]))
            continue
        npu, arena = _load_npu(spec, metadata_root)
        supported = _load_op_support(spec.backend, metadata_root)
        cov, unsupported = op_coverage(ops, supported)
        latency = estimate_latency_ms(macs, npu) if npu else None
        if arena and sram_kib > arena:
            verdict = "no-fit"              # SRAM overflow (dormant until arenas authored)
        elif unsupported:
            verdict = "cpu-fallback"        # unknown/unsupported op -> CPU (never fake-fit)
        else:
            verdict = "fits"
        backends.append(BackendResult(
            backend=spec.backend, verdict=verdict, est_sram_kib=sram_kib,
            budget_sram_kib=(arena or None), est_latency_ms=latency,
            op_coverage_pct=cov, unsupported_ops=unsupported))

    return AnalysisResult(model=str(model_path), sku=sku, backends=backends,
                          suggestion=_suggest(backends))
