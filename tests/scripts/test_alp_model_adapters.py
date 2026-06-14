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
    # compile() is real now (Stage 2); with no per-model config it raises RuntimeError.
    with pytest.raises(RuntimeError, match="config"):
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


@pytest.mark.skipif(shutil.which("vela") is None, reason="vela (ethos-u-vela) not installed")
@pytest.mark.parametrize("accel_config", ["ethos-u85-256", "ethos-u55-256", "ethos-u55-128"])
def test_vela_real_compile_for_e8_accel_configs(tmp_path, accel_config):
    """Compile the committed fixture for each E1M-AEN801 (E8) accel config.

    Proves the shipped Vela accepts every metadata-derived config string for
    the arriving part -- including the E8-only ``ethos-u85-256`` generative NPU
    -- and emits a vela_tflite blob (i.e. op-support + arena sizing).  It does
    NOT prove the blob runs correctly on the U85; that is silicon + Ethos-U HAL
    gated (alp_ethosu_aen_register() returns NOSUPPORT today).
    """
    src = tmp_path / "tiny.tflite"
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite", src)
    blob = VelaAdapter().compile(src, accel_config=accel_config, out_dir=tmp_path)
    assert blob.format == "vela_tflite"
    assert blob.payload[4:8] == b"TFL3"
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


# --- DEEPX dxcom compile (Stage 2 step 2) ---------------------------------

def test_deepx_compile_rejects_missing_config(tmp_path):
    # opts present but no `config` key -> RuntimeError (dxcom needs -c).
    src = tmp_path / "m.onnx"; src.write_bytes(b"ONNX")
    with pytest.raises(RuntimeError, match="config"):
        DeepxAdapter().compile(src, accel_config="", out_dir=tmp_path,
                               opts={"calibration": "calib/"})


def test_deepx_compile_invokes_dxcom_and_returns_dxnn(tmp_path, monkeypatch):
    # A successful dxcom compile writes a single <stem>.dxnn into the -o dir; the
    # adapter returns its raw bytes (blob_format 'dxnn'), NOT a tar of the dir --
    # 'dxnn' is what the device _fmt_enum maps to ALP_INFERENCE_MODEL_DXNN.
    src = tmp_path / "m.onnx"; src.write_bytes(b"ONNX-IN")
    cfg = tmp_path / "m.deepx.json"; cfg.write_text("{}", encoding="utf-8")
    seen = {}

    def fake_run(cmd, capture_output, text, timeout):
        if "-v" in cmd:                              # _dxcom_version() probe
            class _V:
                returncode = 0
                stdout = "DX-COM (DEEPX Compiler) 2.3.0\nTarget Hardware: M1"
                stderr = ""
            return _V()
        seen["cmd"] = cmd                            # the compile invocation
        out = Path(cmd[cmd.index("-o") + 1])         # dxcom writes into the -o dir
        out.mkdir(parents=True, exist_ok=True)
        (out / f"{src.stem}.dxnn").write_bytes(b"DXNN\x08\x00\x00\x00{}")   # canonical artifact
        (out / "compiler.log").write_text("ok", encoding="utf-8")          # stray log to ignore

        class _C:
            returncode = 0
            stdout = ""
            stderr = ""
        return _C()

    monkeypatch.setattr("alp_model.adapters.deepx.subprocess.run", fake_run)
    blob = DeepxAdapter().compile(src, accel_config="", out_dir=tmp_path,
                                  opts={"config": str(cfg)})
    assert seen["cmd"][:3] == ["dxcom", "-m", str(src)]
    assert "-c" in seen["cmd"] and str(cfg) in seen["cmd"]
    assert "-o" in seen["cmd"]
    assert blob.format == "dxnn"
    assert blob.payload.startswith(b"DXNN")          # raw .dxnn flatbuffer, not a tar
    assert blob.compiler_version == "DX-COM 2.3.0"


def test_deepx_compile_raises_when_no_dxnn_produced(tmp_path, monkeypatch):
    src = tmp_path / "m.onnx"; src.write_bytes(b"ONNX-IN")
    cfg = tmp_path / "m.deepx.json"; cfg.write_text("{}", encoding="utf-8")

    def fake_run(cmd, capture_output, text, timeout):
        if "-v" in cmd:
            class _V:
                returncode = 0
                stdout = "DX-COM (DEEPX Compiler) 2.3.0"
                stderr = ""
            return _V()
        Path(cmd[cmd.index("-o") + 1]).mkdir(parents=True, exist_ok=True)   # "succeeds", no .dxnn

        class _C:
            returncode = 0
            stdout = ""
            stderr = ""
        return _C()

    monkeypatch.setattr("alp_model.adapters.deepx.subprocess.run", fake_run)
    with pytest.raises(RuntimeError, match="no .dxnn"):
        DeepxAdapter().compile(src, accel_config="", out_dir=tmp_path, opts={"config": str(cfg)})


@pytest.mark.skipif(shutil.which("dxcom") is None, reason="dxcom (dx-com wheel) not installed")
def test_deepx_real_dxcom_version_smoke():
    # Against the REAL installed dx-com wheel (e.g. a WSL venv): the adapter's
    # version probe reads dxcom's banner. The full real-compile e2e lives in
    # test_deepx_real_compile_of_tiny_fixture (public) + the alp-sdk-internal
    # yolo11n test.
    from alp_model.adapters.deepx import _dxcom_version
    v = _dxcom_version()
    assert v.startswith("DX-COM") and "2.3" in v


def _host_mem_avail_gib() -> float:
    """Available host RAM in GiB (Linux /proc/meminfo); 0.0 if unknown."""
    try:
        with open("/proc/meminfo", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024 * 1024)
    except OSError:
        pass
    return 0.0


# dx-com 2.3.0 aborts in PREPARE with a RamSizeError below ~15 GiB host RAM.
_DXCOM_MIN_RAM_GIB = 15.5


@pytest.mark.skipif(shutil.which("dxcom") is None, reason="dxcom (dx-com wheel) not installed")
@pytest.mark.skipif(_host_mem_avail_gib() < _DXCOM_MIN_RAM_GIB,
                    reason="dxcom needs >15 GiB host RAM (raise WSL via .wslconfig)")
def test_deepx_real_compile_of_tiny_fixture(tmp_path):
    """Compile the committed tiny ONNX with the REAL dxcom -> a single .dxnn.

    Runs only where the licensed dx-com wheel is installed (e.g. a WSL py3.12
    venv: `~/dxcom-venv/bin/python -m pytest tests/scripts/test_alp_model_adapters.py`)
    AND host RAM clears dxcom's ~15 GiB floor; skips otherwise (always in cloud
    CI). Mirrors test_vela_real_compile_of_tiny_fixture. The real-yolo11n
    counterpart lives in tests/scripts/test_deepx_yolo_internal.py (gated on the
    alp-sdk-internal sibling)."""
    import json
    import numpy as np
    from PIL import Image          # Pillow ships as a dx-com wheel dependency

    onnx = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    calib = tmp_path / "calib"
    calib.mkdir()
    rng = np.random.default_rng(0)
    for i in range(4):
        Image.fromarray(rng.integers(0, 256, (224, 224, 3), dtype=np.uint8)).save(calib / f"{i}.png")

    cfg = tmp_path / "tiny.json"
    cfg.write_text(json.dumps({
        "inputs": {"input": [1, 3, 224, 224]},
        "calibration_method": "minmax",
        "calibration_num": 4,
        "default_loader": {
            "dataset_path": str(calib),
            "file_extensions": ["png"],
            "preprocessings": [
                {"resize": {"width": 224, "height": 224}},
                {"normalize": {"mean": [0, 0, 0], "std": [255, 255, 255]}},
                {"transpose": {"axis": [2, 0, 1]}},      # HWC->CHW for the NCHW model
            ],
        },
    }), encoding="utf-8")

    blob = DeepxAdapter().compile(onnx, accel_config="", out_dir=tmp_path,
                                  opts={"config": str(cfg)})
    assert blob.format == "dxnn"
    assert blob.payload[:4] == b"DXNN"        # self-describing .dxnn flatbuffer magic
    assert blob.compiler_version.startswith("DX-COM")
