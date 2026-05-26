# tests/scripts/test_alp_model_adapters.py
"""Compiler adapters: interface + CPU passthrough."""
import pytest
from pathlib import Path
from alp_model.adapters import CompilerAdapter, Blob
from alp_model.adapters.cpu import CpuAdapter
from alp_model.adapters.drpai import DrpaiAdapter
from alp_model.adapters.deepx import DeepxAdapter


def test_cpu_adapter_is_a_compiler_adapter():
    assert issubclass(CpuAdapter, CompilerAdapter)


def test_cpu_adapter_is_always_available_and_accepts_tflite():
    a = CpuAdapter()
    assert a.backend == "cpu"
    assert a.is_available() is True
    assert a.accepts("tflite") is True
    assert a.accepts("onnx") is False        # CPU/TFLM runs tflite only


def test_cpu_adapter_compile_passes_bytes_through(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY-MODEL")
    blob = CpuAdapter().compile(src, accel_config="", out_dir=tmp_path)
    assert isinstance(blob, Blob)
    assert blob.format == "tflite"
    assert blob.payload == b"TFL3-DUMMY-MODEL"
    assert blob.arena_bytes >= 0


def test_drpai_adapter_detect_and_skip(monkeypatch):
    monkeypatch.delenv("ALP_DRPAI_TVM_HOME", raising=False)
    # drpai.py probes the env var only (no shutil.which), so no which-patch needed.
    a = DrpaiAdapter()
    assert issubclass(DrpaiAdapter, CompilerAdapter)
    assert a.backend == "drpai"
    assert a.is_available() is False
    assert a.accepts("onnx") and a.accepts("tflite") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.onnx"), accel_config="", out_dir=Path("."))


def test_deepx_adapter_detect_and_skip(monkeypatch):
    monkeypatch.delenv("ALP_DEEPX_SDK_HOME", raising=False)
    monkeypatch.setattr("alp_model.adapters.deepx.shutil.which", lambda n: None)
    a = DeepxAdapter()
    assert issubclass(DeepxAdapter, CompilerAdapter)
    assert a.backend == "deepx_dxm1"
    assert a.is_available() is False
    assert a.accepts("tflite") and a.accepts("onnx") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.tflite"), accel_config="", out_dir=Path("."))
