# tests/scripts/test_alp_cli_model.py
"""`alp model build` CLI."""
from pathlib import Path
from click.testing import CliRunner
from alp_cli.main import cli

_ROOT = Path(__file__).resolve().parents[2]


def test_alp_model_build_emits_alpmodel(tmp_path):
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.tflite").write_bytes(b"TFL3-DUMMY")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN701\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ])
    assert result.exit_code == 0, result.output
    assert (tmp_path / "out" / "demo.alpmodel").is_file()


def test_alp_model_help_is_registered():
    result = CliRunner().invoke(cli, ["model", "--help"])
    assert result.exit_code == 0
    assert "build" in result.output
