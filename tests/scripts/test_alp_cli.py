from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]


def test_alp_cli_help_lists_subcommands():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    for sub in ("init", "build", "run", "flash", "emit", "validate",
                "model", "doctor", "monitor", "explain", "faultdecode",
                "new-som"):
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
