from click.testing import CliRunner

from alp_cli.main import cli


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
