# tests/scripts/test_alp_cli_model.py
"""`alp model build` CLI."""
import importlib.util
import shutil
from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]


def test_alp_model_build_threads_compile_opts(tmp_path, monkeypatch):
    # CLI must read models[].compile, resolve its paths relative to board.yaml,
    # and pass them to build_model as compile_opts.
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.onnx").write_bytes(b"ONNX")
    (tmp_path / "models" / "m.deepx.json").write_text("{}", encoding="utf-8")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-V2M101\n"
        "cores: {}\n"
        "models:\n"
        "  - name: demo\n"
        "    source: models/m.onnx\n"
        "    compile:\n"
        "      deepx_dxm1: { config: models/m.deepx.json, calibration: models/ }\n",
        encoding="utf-8")
    captured = {}
    import alp_cli.model as climod
    def fake_build_model(*, sku, name, source, out_dir, metadata_root, compile_opts=None):
        captured["compile_opts"] = compile_opts
        p = out_dir / f"{name}.alpmodel"; out_dir.mkdir(parents=True, exist_ok=True); p.write_bytes(b"X")
        return p
    monkeypatch.setattr(climod, "build_model", fake_build_model)
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "build", "--board", str(tmp_path / "board.yaml"),
                                   "--out", str(tmp_path / "out"),
                                   "--metadata-root", str(_ROOT / "metadata")],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    opts = captured["compile_opts"]["deepx_dxm1"]
    assert Path(opts["config"]).is_absolute() and opts["config"].endswith("m.deepx.json")
    assert Path(opts["calibration"]).is_absolute()


def test_alp_model_build_only_resolves_path_valued_drpai_opts(tmp_path, monkeypatch):
    # Issue #1271 root cause: _resolve_compile used to treat EVERY string opt
    # value as a path. drpai's opts mix real paths (images) with opaque
    # strings (input_shape, input_name, product) -- a shape string like
    # "1,3,224,224" resolved into board.yaml's dir became a bogus path
    # (".../1,3,224,224"), which made the adapter's own 224x224 shape check
    # misfire on the only real board.yaml-driven path. Only `images` may be
    # turned into an absolute path here; the rest must reach the adapter
    # byte-for-byte as declared.
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.onnx").write_bytes(b"ONNX")
    (tmp_path / "models" / "calib").mkdir()
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-V2M101\n"
        "cores: {}\n"
        "models:\n"
        "  - name: demo\n"
        "    source: models/m.onnx\n"
        "    compile:\n"
        "      drpai: { input_shape: '1,3,224,224', input_name: input, "
        "images: models/calib, product: V2N }\n",
        encoding="utf-8")
    captured = {}
    import alp_cli.model as climod
    def fake_build_model(*, sku, name, source, out_dir, metadata_root, compile_opts=None):
        captured["compile_opts"] = compile_opts
        p = out_dir / f"{name}.alpmodel"; out_dir.mkdir(parents=True, exist_ok=True); p.write_bytes(b"X")
        return p
    monkeypatch.setattr(climod, "build_model", fake_build_model)
    res = CliRunner().invoke(cli, ["model", "build", "--board", str(tmp_path / "board.yaml"),
                                   "--out", str(tmp_path / "out"),
                                   "--metadata-root", str(_ROOT / "metadata")],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    opts = captured["compile_opts"]["drpai"]
    assert opts["input_shape"] == "1,3,224,224"
    assert opts["input_name"] == "input"
    assert opts["product"] == "V2N"
    assert Path(opts["images"]).is_absolute() and opts["images"].endswith("calib")


def test_alp_model_build_emits_alpmodel(tmp_path):
    (tmp_path / "models").mkdir()
    # A real (compilable) fixture, not dummy bytes: E1M-AEN801 resolves ethos_u
    # targets, so when `vela` is installed (dev/bench boxes, per running-local-ci)
    # the build invokes the real compiler -- which rejects garbage. The tiny
    # fixture compiles on cpu and (when present) vela alike, keeping this green
    # regardless of whether the Ethos-U toolchain is on PATH.
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    assert (tmp_path / "out" / "demo.alpmodel").is_file()


def test_alp_model_build_rejects_a_traversal_model_name(tmp_path):
    # #1125: build_model() itself allowlists models[].name against
    # `^[A-Za-z][A-Za-z0-9_-]*$` (board.schema.json's own pattern) before
    # doing anything else -- the guard lives at the one write chokepoint
    # every caller routes through, not in a separate schema-validation
    # pass `alp model build` would have to remember to run first.
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.tflite").write_bytes(b"TFL3-DUMMY")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n"
        "  - name: '../../../../tmp/evil'\n"
        "    source: models/m.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ])
    assert result.exit_code != 0
    assert not (tmp_path / "out").exists()


def test_alp_model_help_is_registered():
    result = CliRunner().invoke(cli, ["model", "--help"])
    assert result.exit_code == 0
    assert "build" in result.output


def test_alp_model_build_cpu_e2e_with_real_tflite(tmp_path):
    models = tmp_path / "models"
    models.mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite", models / "tiny.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: tiny\n    source: models/tiny.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    mft, blobs = read_package((tmp_path / "out" / "tiny.alpmodel").read_bytes())
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob][4:8] == b"TFL3"          # TFLite flatbuffer file_identifier at offset 4
    if importlib.util.find_spec("tflite"):            # tensor-I/O populated when parser present
        assert mft.inputs and mft.inputs[0].shape == [1, 4]
        assert mft.outputs and mft.outputs[0].shape == [1, 2]
