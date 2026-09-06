# scripts/alp_model/prep.py
"""License-free model prep: validate calibration, INT8-quantize (onnxruntime QDQ
static), and report the fp32-vs-int8 accuracy delta. No vendor toolchain — the
accuracy report is the value (a silent INT8 accuracy cliff is the failure mode
this prevents). Vendor compile (Vela/dxcom/DRP-AI) is a separate later step."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np


class PrepError(Exception):
    """Calibration invalid, or a quantize/inference step failed — raised instead
    of emitting a model that didn't actually quantize or can't run."""


@dataclass(frozen=True)
class CalibrationInfo:
    samples: int
    input_name: str
    input_shape: list[int]


def model_input(onnx_path: Path) -> tuple[str, list[int]]:
    import onnxruntime as ort
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0]
    shape = [(-1 if not isinstance(d, int) else d) for d in inp.shape]
    return inp.name, shape


def _shape_matches(sample: np.ndarray, want: list[int]) -> bool:
    s = list(sample.shape)
    if len(s) != len(want):
        # allow a sample missing the leading batch dim
        if len(s) == len(want) - 1 and want[0] in (-1, 1):
            s = [1, *s]
        else:
            return False
    return all(w in (-1, d) for d, w in zip(s, want))


def load_calibration(cal_dir: Path, input_shape: list[int]) -> list[np.ndarray]:
    files = sorted(Path(cal_dir).glob("*.npy"))
    if not files:
        raise PrepError(f"no .npy calibration samples in {cal_dir}")
    out: list[np.ndarray] = []
    for f in files:
        arr = np.load(f).astype(np.float32)
        if arr.ndim == len(input_shape) - 1:
            arr = arr[None, ...]                       # add batch dim
        if not _shape_matches(arr, input_shape):
            raise PrepError(
                f"calibration sample {f.name} shape {list(arr.shape)} "
                f"!= model input {input_shape}")
        out.append(arr)
    return out


def validate_calibration(cal_dir: Path, onnx_path: Path, *,
                         min_samples: int = 8) -> CalibrationInfo:
    name, shape = model_input(onnx_path)
    samples = load_calibration(cal_dir, shape)
    if len(samples) < min_samples:
        raise PrepError(
            f"calibration set has {len(samples)} samples; need >= {min_samples} "
            f"for a representative INT8 range")
    return CalibrationInfo(samples=len(samples), input_name=name, input_shape=shape)


def _import_quant():
    from onnxruntime.quantization import (CalibrationDataReader, QuantFormat,
                                          QuantType, quantize_static)
    from onnxruntime.quantization.shape_inference import quant_pre_process
    return CalibrationDataReader, QuantFormat, QuantType, quantize_static, quant_pre_process


def _reader_cls(input_name: str, samples: list[np.ndarray]):
    CalibrationDataReader = _import_quant()[0]

    class _NpyReader(CalibrationDataReader):
        def __init__(self) -> None:
            self._it = iter([{input_name: s} for s in samples])

        def get_next(self):
            return next(self._it, None)

    return _NpyReader()


def quantize(raw_onnx: Path, out_onnx: Path, cal_dir: Path, *,
             per_channel: bool = False) -> Path:
    if Path(raw_onnx).suffix.lower() != ".onnx":
        raise PrepError(
            f"model prep supports .onnx input in this release; got {Path(raw_onnx).name} "
            f"(TFLite/PyTorch/Keras -> ONNX conversion is a follow-on)")
    _, _, QuantType, quantize_static, quant_pre_process = _import_quant()
    name, shape = model_input(raw_onnx)
    samples = load_calibration(cal_dir, shape)
    out_onnx = Path(out_onnx)
    out_onnx.parent.mkdir(parents=True, exist_ok=True)
    pre = out_onnx.with_suffix(".pre.onnx")
    try:
        # Symbolic shape inference (the quant_pre_process default) is
        # onnxruntime's recommended pre-process, including for the
        # dynamic-batch models this tool targets. It needs sympy, a model-prep
        # dep (pyproject.toml) -- still license-free: that just means no
        # vendor NPU toolchain (Vela/dxcom/DRP-AI) is involved.
        quant_pre_process(str(raw_onnx), str(pre))
        quantize_static(str(pre), str(out_onnx), _reader_cls(name, samples),
                        per_channel=per_channel, weight_type=QuantType.QInt8)
    except Exception as exc:                      # onnxruntime raises broad types
        raise PrepError(f"quantization failed: {exc}") from exc
    finally:
        if pre.is_file():
            pre.unlink()
    if not out_onnx.is_file():
        raise PrepError("quantization produced no output model")
    return out_onnx


@dataclass(frozen=True)
class AccuracyReport:
    samples: int
    top1_agreement_pct: float
    mean_cosine: float
    max_abs_err: float
    verdict: str
    guidance: str | None


def _cosine(a: np.ndarray, b: np.ndarray) -> float:
    na, nb = float(np.linalg.norm(a)), float(np.linalg.norm(b))
    if na == 0.0 or nb == 0.0:
        return 1.0 if na == nb else 0.0
    return float(np.dot(a, b) / (na * nb))


def accuracy_delta(fp32_onnx: Path, int8_onnx: Path, cal_dir: Path, *,
                   degrade_threshold: float = 95.0) -> AccuracyReport:
    import onnxruntime as ort
    name, shape = model_input(fp32_onnx)
    samples = load_calibration(cal_dir, shape)
    try:
        s32 = ort.InferenceSession(str(fp32_onnx), providers=["CPUExecutionProvider"])
        s8 = ort.InferenceSession(str(int8_onnx), providers=["CPUExecutionProvider"])
        agree, coss, maxerr = 0, [], 0.0
        for x in samples:
            o32 = s32.run(None, {name: x})[0].ravel()
            o8 = s8.run(None, {name: x})[0].ravel()
            if int(o32.argmax()) == int(o8.argmax()):
                agree += 1
            coss.append(_cosine(o32, o8))
            maxerr = max(maxerr, float(np.abs(o32 - o8).max()))
    except Exception as exc:                       # a produced-but-unrunnable
                                                     # model must not escape as
                                                     # a raw traceback
        raise PrepError(f"quantized model failed to run: {exc}") from exc
    top1 = agree / len(samples) * 100.0
    verdict = "good" if top1 >= degrade_threshold else "degraded"
    guidance = None if verdict == "good" else (
        "INT8 accuracy dropped: try --per-channel, add more representative "
        "calibration samples, or keep sensitive ops in fp16.")
    return AccuracyReport(samples=len(samples), top1_agreement_pct=round(top1, 1),
                          mean_cosine=round(float(np.mean(coss)), 4),
                          max_abs_err=round(maxerr, 6), verdict=verdict,
                          guidance=guidance)
