"""Tests for `alp build` -- arg parsing + routing (hermetic, no shell-outs).

The command is a thin wrapper: the orchestrated fan-out and the
single-image `west build` are exercised elsewhere; here we assert the
click surface parses correctly and routes each flag to the right
helper with the right values.
"""

from __future__ import annotations

from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli


def _project(tmp_path: Path) -> Path:
    proj = tmp_path / "proj"
    (proj / "src").mkdir(parents=True)
    (proj / "board.yaml").write_text(
        "som:\n  sku: E1M-AEN701\npreset: e1m-evk\n", encoding="utf-8"
    )
    return proj


def test_build_registered_in_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "build" in result.output


def test_build_help_lists_options():
    result = CliRunner().invoke(cli, ["build", "--help"])
    assert result.exit_code == 0
    for opt in ("--core", "--no-parallel", "--no-validate",
                "--build-root", "--board"):
        assert opt in result.output


def test_build_reports_missing_board_yaml(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, ["build"])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_build_rejects_app_path_without_board_yaml(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "empty").mkdir()
    result = CliRunner().invoke(cli, ["build", str(tmp_path / "empty")])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_build_default_routes_to_fanout(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj / "src")   # found by walking up
    fanout = mocker.patch("alp_cli.build.build_fanout", return_value=0)
    single = mocker.patch("alp_cli.build.build_single_image", return_value=0)
    result = CliRunner().invoke(cli, ["build"])
    assert result.exit_code == 0, result.output
    assert single.call_count == 0
    (args, kwargs) = fanout.call_args
    assert args[0] == proj
    assert kwargs["core"] is None
    assert kwargs["no_parallel"] is False
    assert kwargs["no_validate"] is False
    assert kwargs["build_root"] is None


def test_build_forwards_fanout_flags(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    fanout = mocker.patch("alp_cli.build.build_fanout", return_value=0)
    result = CliRunner().invoke(
        cli,
        ["build", "--core", "m33_sm", "--no-parallel", "--no-validate",
         "--build-root", str(tmp_path / "out")],
    )
    assert result.exit_code == 0, result.output
    (_args, kwargs) = fanout.call_args
    assert kwargs["core"] == "m33_sm"
    assert kwargs["no_parallel"] is True
    assert kwargs["no_validate"] is True
    assert kwargs["build_root"] == tmp_path / "out"


def test_build_board_routes_to_single_image(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    fanout = mocker.patch("alp_cli.build.build_fanout", return_value=0)
    single = mocker.patch("alp_cli.build.build_single_image", return_value=0)
    result = CliRunner().invoke(cli, ["build", "--board", "native_sim"])
    assert result.exit_code == 0, result.output
    assert fanout.call_count == 0
    (args, _kwargs) = single.call_args
    assert args[0] == proj
    assert args[1] == "native_sim"


def test_build_nonzero_rc_propagates(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    mocker.patch("alp_cli.build.build_fanout", return_value=3)
    result = CliRunner().invoke(cli, ["build"])
    assert result.exit_code == 3
