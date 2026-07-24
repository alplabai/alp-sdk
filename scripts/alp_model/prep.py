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
