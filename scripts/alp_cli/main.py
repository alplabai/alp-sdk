"""Top-level click group for the `alp` CLI."""

from __future__ import annotations

import click

from alp_cli import __version__
from alp_cli.init import init_cmd
from alp_cli.run import run_cmd
from alp_cli.validate import validate_cmd


@click.group(help="ALP SDK command-line interface.")
@click.version_option(__version__, prog_name="alp")
def cli() -> None:
    """ALP SDK command-line interface."""


cli.add_command(init_cmd)
cli.add_command(run_cmd)
cli.add_command(validate_cmd)


if __name__ == "__main__":
    cli()
