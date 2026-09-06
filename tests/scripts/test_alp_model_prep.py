"""alp_model.prep — license-free quantize + accuracy report."""
import sys
from pathlib import Path

import numpy as np
import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))
_ONNX = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


def _make_calib(dirpath: Path, n: int, shape=(1, 3, 224, 224)):
    dirpath.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(0)
    for i in range(n):
        np.save(dirpath / f"s{i}.npy", rng.standard_normal(shape).astype(np.float32))


def test_model_input_shape():
    pytest.importorskip("onnxruntime")
    from alp_model.prep import model_input
    name, shape = model_input(_ONNX)
    assert name == "input"
    assert shape[-2:] == [224, 224]


def test_validate_calibration_ok_and_too_few(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import validate_calibration, PrepError
    _make_calib(tmp_path / "cal", 8)
    info = validate_calibration(tmp_path / "cal", _ONNX, min_samples=8)
    assert info.samples == 8 and info.input_name == "input"
    _make_calib(tmp_path / "cal2", 2)
    with pytest.raises(PrepError):
        validate_calibration(tmp_path / "cal2", _ONNX, min_samples=8)


def test_load_calibration_shape_mismatch_raises(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import load_calibration, PrepError
    d = tmp_path / "bad"; d.mkdir()
    np.save(d / "wrong.npy", np.zeros((1, 3, 64, 64), dtype=np.float32))
    with pytest.raises(PrepError):
        load_calibration(d, [1, 3, 224, 224])


def test_quantize_and_accuracy_report(tmp_path):
    pytest.importorskip("onnxruntime")
    from alp_model.prep import quantize, accuracy_delta
    cal = tmp_path / "cal"; _make_calib(cal, 8)
    out = tmp_path / "tiny.int8.onnx"
    q = quantize(_ONNX, out, cal)
    assert q.is_file() and q.stat().st_size > 0
    # the quantized model must actually load + run
    import onnxruntime as ort
    ort.InferenceSession(str(q), providers=["CPUExecutionProvider"])
    rep = accuracy_delta(_ONNX, q, cal)
    assert rep.samples == 8
    assert 0.0 <= rep.top1_agreement_pct <= 100.0
    assert -1.0 <= rep.mean_cosine <= 1.0001
    assert rep.verdict in ("good", "degraded")
    assert (rep.guidance is None) == (rep.verdict == "good")
