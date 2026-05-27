# tests/scripts/test_alp_model_tensorio.py
"""TFLite tensor-I/O extraction into Stage-1a Tensor records."""
from pathlib import Path

import pytest

from alp_model.tensorio import extract_io
from alp_model.manifest import Tensor

_ROOT = Path(__file__).resolve().parents[2]
_FIXTURE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"


def test_extract_io_non_tflite_returns_empty(tmp_path):
    src = tmp_path / "m.onnx"
    src.write_bytes(b"ONNX-BYTES")
    assert extract_io(src) == ([], [])


def test_extract_io_malformed_tflite_returns_empty(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-NOT-REALLY")
    # Contract: an unparseable .tflite always yields no I/O metadata and never raises
    # (parse-exception path when tflite is installed; ImportError path when it isn't).
    assert extract_io(src) == ([], [])


def test_extract_io_parses_tiny_fixture():
    pytest.importorskip("tflite")
    ins, outs = extract_io(_FIXTURE)
    # scale is stored as float32 in the flatbuffer; compare with approx to tolerate
    # the float32 -> float64 precision artifact (e.g. 0.004 -> 0.004000000189989805).
    assert len(ins) == 1
    assert ins[0].dtype == "int8" and ins[0].rank == 2 and ins[0].shape == [1, 4]
    assert ins[0].zp == -1
    assert ins[0].scale == pytest.approx(0.0078125, rel=1e-5)
    assert len(outs) == 1
    assert outs[0].dtype == "int8" and outs[0].rank == 2 and outs[0].shape == [1, 2]
    assert outs[0].zp == 2
    assert outs[0].scale == pytest.approx(0.004, rel=1e-5)
