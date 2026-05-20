from pathlib import Path

from click.testing import CliRunner

from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]


def test_alp_cli_help_lists_subcommands():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    for sub in ("init", "run", "validate"):
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
