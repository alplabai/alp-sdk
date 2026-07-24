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
