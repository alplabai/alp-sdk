# tests/scripts/test_alp_model_adapters.py
"""Compiler adapters: interface + CPU passthrough."""
import shutil
import pytest
from pathlib import Path
from alp_model.adapters import CompilerAdapter, Blob
from alp_model.adapters.cpu import CpuAdapter
from alp_model.adapters.drpai import DrpaiAdapter
from alp_model.adapters.deepx import DeepxAdapter
from alp_model.adapters.ethos_u import VelaAdapter, _parse_vela_summary

_ROOT = Path(__file__).resolve().parents[2]


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
    assert a.accepts("onnx") and not a.accepts("tflite") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.onnx"), accel_config="", out_dir=Path("."))


def test_deepx_adapter_detect_and_skip(monkeypatch):
    monkeypatch.delenv("ALP_DEEPX_SDK_HOME", raising=False)
    monkeypatch.setattr("alp_model.adapters.deepx.shutil.which", lambda n: None)
    a = DeepxAdapter()
    assert issubclass(DeepxAdapter, CompilerAdapter)
    assert a.backend == "deepx_dxm1"
    assert a.is_available() is False
    assert a.accepts("onnx") and not a.accepts("tflite") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.onnx"), accel_config="", out_dir=Path("."))


def test_vela_adapter_backend_and_accepts():
    a = VelaAdapter()
    assert a.backend == "ethos_u"
    assert a.accepts("tflite") and not a.accepts("onnx")


def test_vela_adapter_is_available_follows_path(monkeypatch):
    monkeypatch.setattr("alp_model.adapters.ethos_u.shutil.which", lambda n: None)
    assert VelaAdapter().is_available() is False
    monkeypatch.setattr("alp_model.adapters.ethos_u.shutil.which", lambda n: "/usr/bin/vela")
    assert VelaAdapter().is_available() is True


def test_vela_adapter_compile_invokes_cli_and_reads_output(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-INPUT")
    seen = {}

    def fake_run(cmd, capture_output, text, timeout):
        seen["cmd"] = cmd
        (tmp_path / "m_vela.tflite").write_bytes(b"VELA-OUT")   # emulate vela's output

        class _R:
            returncode = 0
            stdout = ""
            stderr = ""
        return _R()

    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)
    assert seen["cmd"][:2] == ["vela", str(src)]
    assert "--accelerator-config" in seen["cmd"] and "ethos-u55-128" in seen["cmd"]
    assert blob.format == "vela_tflite"
    assert blob.payload == b"VELA-OUT"
    assert blob.compiler_version.startswith("vela")


def test_vela_adapter_compile_raises_on_vela_error(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-INPUT")

    def fake_run(cmd, capture_output, text, timeout):
        class _R:
            returncode = 1
            stdout = ""
            stderr = "Invalid model"
        return _R()

    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    with pytest.raises(RuntimeError, match="vela failed"):
        VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)


def test_vela_adapter_compile_raises_when_output_file_missing(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-INPUT")

    def fake_run(cmd, capture_output, text, timeout):
        class _R:
            returncode = 0
            stdout = ""
            stderr = ""
        return _R()                        # vela "succeeds" but writes no _vela.tflite

    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    with pytest.raises(RuntimeError, match="produced no output"):
        VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)


def test_parse_vela_summary_extracts_sram_and_arena(tmp_path):
    (tmp_path / "m_summary_internal.csv").write_text(
        "network,sram_memory_used,arena_cache_size\n"
        "m,262144,131072\n", encoding="utf-8")
    arena, sram_kib = _parse_vela_summary(tmp_path, "m")
    assert sram_kib == 256        # 262144 bytes -> 256 KiB
    assert arena == 131072


def test_parse_vela_summary_absent_returns_zeros(tmp_path):
    assert _parse_vela_summary(tmp_path, "missing") == (0, 0)


@pytest.mark.skipif(shutil.which("vela") is None, reason="vela (ethos-u-vela) not installed")
def test_vela_real_compile_of_tiny_fixture(tmp_path):
    src = tmp_path / "tiny.tflite"
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite", src)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)
    assert blob.format == "vela_tflite"
    assert blob.payload[4:8] == b"TFL3"        # vela emits a .tflite flatbuffer
    assert blob.compiler_version.startswith("vela")


def test_cpu_and_vela_do_not_require_compile_opts():
    assert CpuAdapter().requires_compile_opts is False
    assert VelaAdapter().requires_compile_opts is False


def test_drpai_and_deepx_require_compile_opts():
    assert DrpaiAdapter().requires_compile_opts is True
    assert DeepxAdapter().requires_compile_opts is True


def test_cpu_compile_accepts_opts_kwarg(tmp_path):
    src = tmp_path / "m.tflite"; src.write_bytes(b"TFL3-X")
    blob = CpuAdapter().compile(src, accel_config="", out_dir=tmp_path, opts=None)
    assert blob.payload == b"TFL3-X"


def test_vela_compile_accepts_opts_kwarg(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"; src.write_bytes(b"TFL3-X")
    def fake_run(cmd, capture_output, text, timeout):
        (tmp_path / "m_vela.tflite").write_bytes(b"VELA-OUT")
        class _R: returncode = 0; stdout = ""; stderr = ""
        return _R()
    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128",
                                 out_dir=tmp_path, opts={"ignored": True})
    assert blob.payload == b"VELA-OUT"
