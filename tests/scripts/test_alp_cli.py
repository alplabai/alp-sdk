from pathlib import Path
from types import SimpleNamespace

import pytest
from click.testing import CliRunner

from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]

# The west-extension wrappers: `alp <verb>` delegates to
# scripts/west_commands/alp_<verb>.py run standalone.
_WRAPPED_WEST_VERBS = ("size", "clean", "image", "renode")


def test_alp_cli_help_lists_subcommands():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    for sub in ("init", "build", "run", "flash", "emit", "validate",
                "model", "doctor", "monitor", "explain", "faultdecode",
                "new-som", "size", "clean", "image", "renode"):
        assert sub in result.output


def test_alp_cli_reports_version():
    from alp_cli import __version__

    result = CliRunner().invoke(cli, ["--version"])
    assert result.exit_code == 0
    assert __version__ in result.output


def test_validate_passes_on_good_fixture():
    good = REPO / "tests" / "fixtures" / "board_yaml_good" / "minimal.yaml"
    result = CliRunner().invoke(cli, ["validate", str(good)])
    assert result.exit_code == 0


def test_validate_fails_on_bad_fixture_and_prints_code():
    bad = REPO / "tests" / "fixtures" / "board_yaml_bad" / "ALP-B001-missing-required.yaml"
    result = CliRunner().invoke(cli, ["validate", str(bad)])
    assert result.exit_code != 0
    assert "ALP-B001" in result.output


def test_init_non_interactive_scaffolds_project(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(
        cli,
        ["init", "my-app", "--som", "E1M-AEN701", "--preset", "e1m-evk",
         "--peripherals", "uart,gpio"],
    )
    assert result.exit_code == 0, result.output
    proj = tmp_path / "my-app"
    assert (proj / "board.yaml").is_file()
    assert (proj / "src" / "main.c").is_file()
    assert (proj / "CMakeLists.txt").is_file()
    board_yaml = (proj / "board.yaml").read_text(encoding="utf-8")
    assert "E1M-AEN701" in board_yaml
    assert "e1m-evk" in board_yaml


def test_init_refuses_existing_directory(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "already-there").mkdir()
    result = CliRunner().invoke(
        cli,
        ["init", "already-there", "--som", "E1M-AEN701", "--preset", "e1m-evk"],
    )
    assert result.exit_code != 0
    assert "already exists" in result.output


def test_run_reports_missing_board_yaml(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_run_finds_board_yaml_from_subdirectory(tmp_path: Path, monkeypatch, mocker):
    monkeypatch.chdir(tmp_path)
    proj = tmp_path / "proj"
    (proj / "src").mkdir(parents=True)
    (proj / "board.yaml").write_text("som:\n  sku: E1M-AEN701\npreset: e1m-evk\n")
    subdir = proj / "src"
    monkeypatch.chdir(subdir)
    # Mock the actual build/exec so the test doesn't shell out.
    called = {}

    def _stub(project_dir):
        called["dir"] = project_dir
        return 0

    mocker.patch("alp_cli.run._build_and_exec_native_sim", side_effect=_stub)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code == 0
    assert called["dir"] == proj


# ---------------------------------------------------------------------
# `alp size` / `alp clean` / `alp image` / `alp renode` -- the west
# extension wrappers that complete the single-front-door verb set.
# ---------------------------------------------------------------------


def _project(tmp_path: Path) -> Path:
    proj = tmp_path / "proj"
    proj.mkdir()
    (proj / "board.yaml").write_text(
        "som:\n  sku: E1M-AEN701\npreset: e1m-evk\n", encoding="utf-8"
    )
    return proj


@pytest.mark.parametrize("verb", _WRAPPED_WEST_VERBS)
def test_wrapped_verb_help_exits_zero(verb):
    result = CliRunner().invoke(cli, [verb, "--help"])
    assert result.exit_code == 0
    # Every wrapper names its west sibling, so users can map the docs.
    assert f"west alp-{verb}" in result.output


@pytest.mark.parametrize("verb", _WRAPPED_WEST_VERBS)
def test_wrapped_verb_reports_missing_board_yaml(verb, tmp_path: Path,
                                                 monkeypatch):
    """Outside any project (and any west workspace): a clean one-line
    error, never a traceback."""
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, [verb])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output
    assert result.exception is None or isinstance(result.exception,
                                                  SystemExit)


@pytest.mark.parametrize(
    "verb,flags,expected",
    [
        ("size", ["--json", "--fail-over-budget"],
         ["--json", "--fail-over-budget"]),
        ("clean", ["--dry-run"], ["--dry-run"]),
        ("image", [], []),
        ("renode", ["--expect", "[hello] done", "--timeout", "30"],
         ["--expect", "[hello] done", "--timeout", "30"]),
    ],
)
def test_wrapped_verb_delegates_to_west_module(verb, flags, expected,
                                               tmp_path: Path, monkeypatch,
                                               mocker):
    """Each wrapper shells out to scripts/west_commands/alp_<verb>.py
    (standalone main), forwarding its options verbatim."""
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    # A stray ALP_SDK_ROOT pointing at another checkout would redirect
    # sdk_root() and fail the REPO-path assert spuriously.
    monkeypatch.delenv("ALP_SDK_ROOT", raising=False)
    run = mocker.patch(
        f"alp_cli.{verb}.subprocess.run",
        return_value=SimpleNamespace(returncode=0),
    )
    result = CliRunner().invoke(cli, [verb, *flags])
    assert result.exit_code == 0, result.output
    cmd = run.call_args[0][0]
    assert str(REPO / "scripts" / "west_commands" / f"alp_{verb}.py") in cmd
    assert str(proj) in cmd
    for token in expected:
        assert token in cmd


@pytest.mark.parametrize("verb", _WRAPPED_WEST_VERBS)
def test_wrapped_verb_propagates_nonzero_rc(verb, tmp_path: Path,
                                            monkeypatch, mocker):
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    mocker.patch(
        f"alp_cli.{verb}.subprocess.run",
        return_value=SimpleNamespace(returncode=3),
    )
    result = CliRunner().invoke(cli, [verb])
    assert result.exit_code == 3


def test_size_real_delegation_fails_cleanly_without_manifest(
        tmp_path: Path, monkeypatch):
    """End-to-end (real subprocess): `alp size` on an unbuilt project
    exercises the standalone alp_size.py main and exits non-zero."""
    proj = _project(tmp_path)
    monkeypatch.chdir(proj)
    result = CliRunner().invoke(cli, ["size"])
    assert result.exit_code != 0
