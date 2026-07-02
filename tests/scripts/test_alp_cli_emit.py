"""Tests for `alp emit` -- arg parsing + the alp_project.py delegation.

Hermetic: the actual subprocess is mocked; what we assert is that the
click surface accepts exactly the modes `scripts/alp_project.py`
accepts and builds the right delegated command line.
"""

from __future__ import annotations

import re
from pathlib import Path
from types import SimpleNamespace

from click.testing import CliRunner

from alp_cli.emit import EMIT_MODES
from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]


def _project(tmp_path: Path) -> Path:
    proj = tmp_path / "proj"
    proj.mkdir()
    (proj / "board.yaml").write_text(
        "som:\n  sku: E1M-AEN701\npreset: e1m-evk\n", encoding="utf-8"
    )
    return proj


def test_emit_registered_in_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "emit" in result.output


def test_emit_modes_match_alp_project_choices():
    """Drift guard: the CLI's mode list mirrors alp_project.py's --emit
    choices (harvested from the argparse source, quoted strings inside
    the choices=[...] literal)."""
    src = (REPO / "scripts" / "alp_project.py").read_text(encoding="utf-8")
    m = re.search(r"--emit\",\s*choices=\[(.*?)\]", src, re.DOTALL)
    assert m, "could not locate the --emit choices list in alp_project.py"
    choices = re.findall(r'"([a-z0-9-]+)"', m.group(1))
    assert sorted(choices) == sorted(EMIT_MODES)


def test_emit_rejects_unknown_mode():
    result = CliRunner().invoke(cli, ["emit", "not-a-mode"])
    assert result.exit_code != 0
    assert "not-a-mode" in result.output


def test_emit_requires_board_yaml_or_input(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, ["emit", "zephyr-conf"])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_emit_delegates_to_alp_project(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    # A stray ALP_SDK_ROOT pointing at another checkout would redirect
    # sdk_root() and fail the REPO-path assert spuriously.
    monkeypatch.delenv("ALP_SDK_ROOT", raising=False)
    run = mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=0),
    )
    result = CliRunner().invoke(cli, ["emit", "system-manifest"])
    assert result.exit_code == 0, result.output
    cmd = run.call_args[0][0]
    assert str(REPO / "scripts" / "alp_project.py") in cmd
    assert cmd[cmd.index("--emit") + 1] == "system-manifest"
    assert cmd[cmd.index("--input") + 1] == str(proj / "board.yaml")
    assert "--output" not in cmd
    assert "--core" not in cmd


def test_emit_forwards_input_output_core(tmp_path, mocker, monkeypatch):
    proj = _project(tmp_path)
    monkeypatch.chdir(tmp_path)          # NOT the project dir: --input wins
    run = mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=0),
    )
    out = tmp_path / "out.conf"
    result = CliRunner().invoke(
        cli,
        ["emit", "zephyr-conf", "--input", str(proj / "board.yaml"),
         "--output", str(out), "--core", "m55_he"],
    )
    assert result.exit_code == 0, result.output
    cmd = run.call_args[0][0]
    assert cmd[cmd.index("--input") + 1] == str(proj / "board.yaml")
    assert cmd[cmd.index("--output") + 1] == str(out)
    assert cmd[cmd.index("--core") + 1] == "m55_he"


def test_emit_nonzero_rc_propagates(tmp_path, monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=2),
    )
    result = CliRunner().invoke(cli, ["emit", "zephyr-conf"])
    assert result.exit_code == 2
