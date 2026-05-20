"""Top-level click group for the `alp` CLI."""

from __future__ import annotations

import click

from alp_cli import __version__
from alp_cli.validate import validate_cmd


@click.group(help="ALP SDK command-line interface.")
@click.version_option(__version__, prog_name="alp")
def cli() -> None:
    """ALP SDK command-line interface."""


@cli.command(help="(stub) Scaffold a new project.")
def init() -> None:
    click.echo("init: not yet implemented")


@cli.command(help="(stub) Build and run on native_sim.")
def run() -> None:
    click.echo("run: not yet implemented")


cli.add_command(validate_cmd)


if __name__ == "__main__":
    cli()
