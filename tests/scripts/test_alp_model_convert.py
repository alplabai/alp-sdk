"""alp_model.convert — TFLite -> ONNX."""
import sys
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_ROOT / "scripts"))
_TFLITE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
_ONNX = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"


def test_onnx_passthrough(tmp_path):
    from alp_model.convert import to_onnx
    out = to_onnx(_ONNX, tmp_path / "x.onnx")
    assert out == _ONNX                       # unchanged, no conversion


def test_bad_suffix_raises(tmp_path):
    from alp_model.convert import to_onnx, ConvertError
    src = tmp_path / "m.pt"; src.write_bytes(b"x")
    with pytest.raises(ConvertError):
        to_onnx(src, tmp_path / "o.onnx")


def test_tflite_to_onnx_converts_and_loads(tmp_path):
    pytest.importorskip("tf2onnx")
    pytest.importorskip("tensorflow")
    import onnxruntime as ort
    from alp_model.convert import to_onnx
    out = to_onnx(_TFLITE, tmp_path / "conv.onnx")
    assert out.is_file() and out.stat().st_size > 0
    ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])  # must load
