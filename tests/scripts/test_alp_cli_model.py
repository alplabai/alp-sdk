# tests/scripts/test_alp_cli_model.py
"""`alp model build` CLI."""
import importlib.util
import shutil
from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]


def test_model_zoo_json_lists_example():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "zoo", "--format", "json"], catch_exceptions=False)
    assert res.exit_code == 0, res.output
    ids = [e["id"] for e in json.loads(res.output)["entries"]]
    assert "example-tiny" in ids


def test_model_zoo_sku_marks_runs_here():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "zoo", "--sku", "E1M-AEN801", "--format", "json"],
                             catch_exceptions=False)
    entry = next(e for e in json.loads(res.output)["entries"] if e["id"] == "example-tiny")
    assert entry["runs_here"] is True


def test_model_add_appends_bundled_to_board(tmp_path):
    import yaml as _yaml
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text("som:\n  sku: E1M-AEN801\ncores: {}\n", encoding="utf-8")
    res = CliRunner().invoke(
        cli, ["model", "add", "example-tiny", "--board", str(board)],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    doc = _yaml.safe_load(board.read_text("utf-8"))
    names = [m["name"] for m in doc.get("models", [])]
    assert "example-tiny" in names
    entry = next(m for m in doc["models"] if m["name"] == "example-tiny")
    assert (tmp_path / entry["source"]).is_file()  # source resolved + fetched


def test_model_add_duplicate_name_errors(tmp_path):
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text("som:\n  sku: E1M-AEN801\ncores: {}\n", encoding="utf-8")
    args = ["model", "add", "example-tiny", "--board", str(board)]
    CliRunner().invoke(cli, args, catch_exceptions=False)
    res2 = CliRunner().invoke(cli, args)  # second add of the same name
    assert res2.exit_code != 0


def test_model_add_preserves_existing_board_and_models(tmp_path):
    import yaml as _yaml
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text(
        "name: myboard\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: keep-me\n    source: keep.tflite\n",
        encoding="utf-8")
    res = CliRunner().invoke(
        cli, ["model", "add", "example-tiny", "--board", str(board)],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    doc = _yaml.safe_load(board.read_text("utf-8"))
    assert doc["name"] == "myboard"
    names = [m["name"] for m in doc["models"]]
    assert "keep-me" in names
    assert "example-tiny" in names


def test_model_add_empty_models_key_ok(tmp_path):
    import yaml as _yaml
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text(
        "som:\n  sku: E1M-AEN801\ncores: {}\nmodels:\n", encoding="utf-8")
    res = CliRunner().invoke(
        cli, ["model", "add", "example-tiny", "--board", str(board)],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    doc = _yaml.safe_load(board.read_text("utf-8"))
    names = [m["name"] for m in doc["models"]]
    assert "example-tiny" in names


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
    assert model["source"] == "models/m.tflite"    # raw relative string, not the resolved absolute path
    assert model["alpmodel_path"].endswith("demo.alpmodel")
    assert model["total_bytes"] > 0
    cpu = [t for t in model["targets"] if t["backend"] == "cpu"]
    assert len(cpu) == 1 and cpu[0]["blob_bytes"] > 0
    assert set(cpu[0].keys()) == {
        "backend", "silicon_ref", "blob_format", "accel_config",
        "arena", "blob_bytes", "requires", "compiler_version",
    }
    assert "blob" not in cpu[0]
    assert "backend_id" not in cpu[0]
    # ethos_u is a declared AEN801 target; without vela on PATH it is a skip.
    assert all(s["status"] in ("skipped", "incompatible") for s in model["skipped"])


def test_alp_model_build_json_reports_failure_as_json(tmp_path):
    # source: points at a file that does not exist -> read_bytes() raises OSError
    # deep inside build_model (CpuAdapter.compile), not the ValueError build_model
    # itself raises for "no blob compiled". The json path must still emit a JSON
    # envelope on stdout with the failure recorded, not an escaping traceback.
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/missing.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--format", "json",
    ])
    assert result.exit_code == 1
    payload = _json.loads(result.output)
    model = payload["models"][0]
    assert model["name"] == "demo"
    assert model["source"] == "models/missing.tflite"
    assert model["error"]
    assert model["targets"] == []
    assert model["skipped"] == []


def test_alp_model_build_dash_model_selects_one(tmp_path):
    # Two models declared; --model demo must build ONLY demo, and --model nope
    # (unknown name) must fail clearly instead of building everything.
    (tmp_path / "models").mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m2.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n"
        "  - name: demo\n    source: models/m.tflite\n"
        "  - name: other\n    source: models/m2.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--model", "demo",
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    payload = _json.loads(result.output)
    assert len(payload["models"]) == 1
    assert payload["models"][0]["name"] == "demo"
    assert not (tmp_path / "out" / "other.alpmodel").exists()

    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--model", "nope",
        "--format", "json",
    ])
    assert result.exit_code == 1


def test_alp_model_build_dash_model_selects_one_human_format(tmp_path):
    # Same as test_alp_model_build_dash_model_selects_one, but the default
    # (human) output format -- the untested path per Plan A review.
    (tmp_path / "models").mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m2.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n"
        "  - name: demo\n    source: models/m.tflite\n"
        "  - name: other\n    source: models/m2.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--model", "demo",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    assert (tmp_path / "out" / "demo.alpmodel").is_file()
    assert not (tmp_path / "out" / "other.alpmodel").exists()

    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--model", "nope",
    ])
    assert result.exit_code == 1


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
    build_result = CliRunner().invoke(cli, [
        "model", "build", "--board", str(tmp_path / "board.yaml"),
        "--out", str(out), "--metadata-root", str(_ROOT / "metadata"),
        "--format", "json",
    ], catch_exceptions=False)
    build_cpu = next(t for t in _json.loads(build_result.output)["models"][0]["targets"]
                     if t["backend"] == "cpu")
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
    info_cpu = next(t for t in info["targets"] if t["backend"] == "cpu")
    assert "blob" not in info_cpu
    assert set(info_cpu.keys()) == set(build_cpu.keys())   # info/build target shape parity
    matrix = {row["backend"]: row["has_blob"] for row in info["coverage_matrix"]}
    assert matrix["cpu"] is True          # cpu always compiles
    assert "ethos_u" in matrix            # declared AEN801 backend appears in the matrix


def test_alp_model_info_missing_artifact_errors(tmp_path):
    result = CliRunner().invoke(cli, [
        "model", "info", "nope", "--out", str(tmp_path), "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 1


def test_model_check_json(tmp_path):
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801", "--format", "json"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert payload["sku"] == "E1M-AEN801"
    assert payload["backends"], "expected backends"
    assert all(b["source"] == "static" for b in payload["backends"])
    cpu = [b for b in payload["backends"] if b["backend"] == "cpu"]
    assert cpu and cpu[0]["verdict"] == "fits"


def test_model_check_human_lists_backends():
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    assert "ethos_u" in res.output and "cpu" in res.output
    assert "static" in res.output


def test_model_check_non_tflite_errors():
    from click.testing import CliRunner
    from alp_cli.main import cli
    model = _ROOT / "tests/fixtures/models/tiny_cnn.onnx"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--sku", "E1M-AEN801"])
    assert res.exit_code == 1
    assert "tflite" in res.output.lower()


def test_model_check_board_mode_json():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board.yaml"
    res = CliRunner().invoke(
        cli, ["model", "check", "--board", str(board), "--format", "json"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert payload["board"] == str(board)
    assert payload["sku"] == "E1M-AEN801"
    assert payload["models"]
    m = payload["models"][0]
    assert m["name"] == "tiny"
    assert m["backends"]
    cpu = [b for b in m["backends"] if b["backend"] == "cpu"]
    assert cpu and cpu[0]["verdict"] == "fits"


def test_model_check_board_and_positional_are_exclusive():
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board.yaml"
    model = _ROOT / "tests/fixtures/models/tiny_int8.tflite"
    res = CliRunner().invoke(
        cli, ["model", "check", str(model), "--board", str(board)])
    assert res.exit_code != 0
    assert "exclusive" in res.output.lower()


def test_model_check_board_single_model_selector():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board.yaml"
    res = CliRunner().invoke(
        cli, ["model", "check", "--board", str(board), "--model", "tiny",
              "--format", "json"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert len(payload["models"]) == 1
    assert payload["models"][0]["name"] == "tiny"


def test_model_check_board_mode_bad_source_is_per_model_error():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board_badsource.yaml"
    res = CliRunner().invoke(
        cli, ["model", "check", "--board", str(board), "--format", "json"])
    assert res.exit_code == 1, res.output      # per-model failure, not a usage error
    payload = json.loads(res.output)           # payload still printed despite the failure
    m = payload["models"][0]
    assert m["name"] == "bad"
    assert m["source"] == "../models/tiny_cnn.onnx"
    assert m["error"]
    assert "backends" not in m


def test_model_check_board_unknown_model_selector_errors():
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board.yaml"
    res = CliRunner().invoke(
        cli, ["model", "check", "--board", str(board), "--model", "no_such_name"])
    assert res.exit_code != 0
    assert "no_such_name" in res.output


def test_model_check_board_explicit_sku_overrides_board():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = _ROOT / "tests/fixtures/boards/check_board.yaml"
    res = CliRunner().invoke(
        cli, ["model", "check", "--board", str(board), "--sku", "E1M-V2N101",
              "--format", "json"],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    payload = json.loads(res.output)
    assert payload["sku"] == "E1M-V2N101"
    backends = {b["backend"] for b in payload["models"][0]["backends"]}
    assert "drpai" in backends
