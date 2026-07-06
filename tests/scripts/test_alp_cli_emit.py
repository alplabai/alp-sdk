"""Tests for `alp emit` -- arg parsing + the two delegation paths.

Hermetic: the actual subprocess is mocked; what we assert is that the
click surface accepts the union of `scripts/alp_project.py --emit` and
`west alp-emit` modes (the single-front-door superset contract) and
builds the right delegated command line for each.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from types import SimpleNamespace

from click.testing import CliRunner

from alp_cli.emit import EMIT_MODES, ORCHESTRATOR_EMIT_MODES, PROJECT_EMIT_MODES
from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]

# The west alp-emit command module is import-safe without west, so the
# parity tests can read its mode list straight from the source of truth.
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))
import alp_emit as west_alp_emit  # noqa: E402


def _harvest_choices(source: Path, flag: str = "--emit") -> list[str]:
    """Pull the quoted strings out of an argparse ``choices=[...]``
    literal following *flag* in *source* (regex-harvested so the test
    tracks the real CLI, not a hand-copied list)."""
    src = source.read_text(encoding="utf-8")
    m = re.search(re.escape(flag) + r"\",\s*(?:default=None,\s*)?choices=\[(.*?)\]",
                  src, re.DOTALL)
    assert m, f"could not locate the {flag} choices list in {source}"
    return re.findall(r'"([a-z0-9-]+)"', m.group(1))


def _project(tmp_path: Path) -> Path:
    proj = tmp_path / "proj"
    proj.mkdir()
    (proj / "board.yaml").write_text(
        "som:\n  sku: E1M-AEN801\npreset: e1m-evk\n", encoding="utf-8"
    )
    return proj


def test_emit_registered_in_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "emit" in result.output


def test_project_emit_modes_match_alp_project_choices():
    """Drift guard: the CLI's project-mode list mirrors alp_project.py's
    --emit choices (harvested from the argparse source)."""
    choices = _harvest_choices(REPO / "scripts" / "alp_project.py")
    assert sorted(choices) == sorted(PROJECT_EMIT_MODES)


def test_orchestrator_emit_modes_match_orchestrator_choices():
    """Drift guard: the CLI's orchestrator-mode list mirrors the real
    `python -m alp_orchestrate` --emit choices."""
    choices = _harvest_choices(REPO / "scripts" / "alp_orchestrate" / "cli.py")
    assert sorted(choices) == sorted(ORCHESTRATOR_EMIT_MODES)


def test_alp_emit_is_superset_of_west_alp_emit():
    """THE front-door parity contract: every mode `west alp-emit`
    accepts must be reachable from `alp emit`.  Both lists are derived
    programmatically (the CLI's registered choices vs the west command
    module's _EMIT_MODES constant), so this test cannot drift."""
    west_modes = set(west_alp_emit._EMIT_MODES)
    alp_modes = set(EMIT_MODES)
    missing = west_modes - alp_modes
    assert not missing, (
        f"west alp-emit modes unreachable from alp emit: {sorted(missing)}")
    # And the click surface really registers the full union.
    from alp_cli.emit import emit_cmd
    mode_param = next(p for p in emit_cmd.params if p.name == "mode")
    assert west_modes <= set(mode_param.type.choices)


def test_west_alp_emit_mirror_matches_orchestrator():
    """The west command's hand-mirrored list must track the orchestrator
    CLI it delegates to (completes the three-way drift guard)."""
    choices = _harvest_choices(REPO / "scripts" / "alp_orchestrate" / "cli.py")
    assert sorted(west_alp_emit._EMIT_MODES) == sorted(choices)


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


def test_emit_orchestrator_mode_delegates_to_alp_orchestrate(
        tmp_path, monkeypatch, mocker):
    """Orchestrator-only modes go through `python -m alp_orchestrate
    --emit <mode>` -- the exact invocation `west alp-emit` performs."""
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    run = mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=0),
    )
    result = CliRunner().invoke(
        cli, ["emit", "build-plan", "--build-root", "out"])
    assert result.exit_code == 0, result.output
    cmd = run.call_args[0][0]
    assert cmd[cmd.index("-m") + 1] == "alp_orchestrate"
    assert cmd[cmd.index("--emit") + 1] == "build-plan"
    assert cmd[cmd.index("--input") + 1] == str(proj / "board.yaml")
    assert cmd[cmd.index("--build-root") + 1] == "out"


def test_emit_orchestrator_mode_output_written(tmp_path, monkeypatch, mocker):
    """--output for an orchestrator mode is CLI plumbing: capture the
    delegated stdout, write it to the file."""
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=0,
                                     stdout="partitions {};\n", stderr=""),
    )
    out = tmp_path / "gen" / "partitions.overlay"
    result = CliRunner().invoke(
        cli, ["emit", "dts-partitions", "--output", str(out)])
    assert result.exit_code == 0, result.output
    assert out.read_text(encoding="utf-8") == "partitions {};\n"


def test_emit_orchestrator_mode_warns_core_ignored(tmp_path, monkeypatch,
                                                   mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    run = mocker.patch(
        "alp_cli.emit.subprocess.run",
        return_value=SimpleNamespace(returncode=0),
    )
    result = CliRunner().invoke(
        cli, ["emit", "storage-mounts-c", "--core", "m55_he"])
    assert result.exit_code == 0, result.output
    assert "--core is ignored" in result.output
    assert "--core" not in run.call_args[0][0]
