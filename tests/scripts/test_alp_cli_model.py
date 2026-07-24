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


import json as _json


def test_alp_model_build_json_emits_targets_and_coverage(tmp_path):
    (tmp_path / "models").mkdir()
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
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    payload = _json.loads(result.output)
    model = payload["models"][0]
    assert model["name"] == "demo"
    assert model["alpmodel_path"].endswith("demo.alpmodel")
    assert model["total_bytes"] > 0
    cpu = [t for t in model["targets"] if t["backend"] == "cpu"]
    assert len(cpu) == 1 and cpu[0]["blob_bytes"] > 0
    # ethos_u is a declared AEN801 target; without vela on PATH it is a skip.
    assert all(s["status"] in ("skipped", "incompatible") for s in model["skipped"])


def test_alp_model_list_reports_artifact_status(tmp_path):
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.tflite").write_bytes(b"TFL3xxxx")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    out = tmp_path / "build" / "models"
    out.mkdir(parents=True)
    (out / "demo.alpmodel").write_bytes(b"ALPM....")   # newer than source
    result = CliRunner().invoke(cli, [
        "model", "list",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(out),
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    m = _json.loads(result.output)["models"][0]
    assert m["name"] == "demo"
    assert m["artifact"]["exists"] is True
    assert m["artifact"]["stale"] is False


def test_alp_model_doctor_lists_all_backends():
    result = CliRunner().invoke(cli, ["model", "doctor", "--format", "json"],
                                catch_exceptions=False)
    assert result.exit_code == 0, result.output
    backends = {t["backend"] for t in _json.loads(result.output)["toolchains"]}
    assert {"cpu", "ethos_u", "drpai", "deepx_dxm1"} <= backends
    cpu = next(t for t in _json.loads(result.output)["toolchains"] if t["backend"] == "cpu")
    assert cpu["available"] is True


def test_alp_model_info_decodes_manifest_and_matrix(tmp_path):
    (tmp_path / "models").mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    out = tmp_path / "out"
    CliRunner().invoke(cli, [
        "model", "build", "--board", str(tmp_path / "board.yaml"),
        "--out", str(out), "--metadata-root", str(_ROOT / "metadata"),
    ], catch_exceptions=False)
    result = CliRunner().invoke(cli, [
        "model", "info", "demo",
        "--out", str(out),
        "--board", str(tmp_path / "board.yaml"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    info = _json.loads(result.output)
    assert info["name"] == "demo"
    assert any(t["backend"] == "cpu" for t in info["targets"])
    matrix = {row["backend"]: row["has_blob"] for row in info["coverage_matrix"]}
    assert matrix["cpu"] is True          # cpu always compiles
    assert "ethos_u" in matrix            # declared AEN801 backend appears in the matrix


def test_alp_model_info_missing_artifact_errors(tmp_path):
    result = CliRunner().invoke(cli, [
        "model", "info", "nope", "--out", str(tmp_path), "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 1
