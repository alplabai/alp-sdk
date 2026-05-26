# tests/scripts/test_alp_model_adapters.py
"""Compiler adapters: interface + CPU passthrough."""
from alp_model.adapters import CompilerAdapter, Blob
from alp_model.adapters.cpu import CpuAdapter


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
