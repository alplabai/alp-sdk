"""`alp validate` -- run the board.yaml validator on a file."""

from __future__ import annotations

from pathlib import Path

import click

from alp_cli.diagnostic import render
from alp_cli.validator import validate_board_yaml
from alp_orchestrate import OrchestratorError, load_board_yaml


@click.command(name="validate", help="Validate a board.yaml file.")
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
                required=False, default=Path("board.yaml"))
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
def validate_cmd(path: Path, no_color: bool) -> None:
    collector = validate_board_yaml(path)
    source_text = path.read_text(encoding="utf-8")
    for diag in collector:
        click.echo(render(diag, source_text=source_text, color=not no_color))
    if collector.has_errors():
        raise SystemExit(1)
    try:
        load_board_yaml(path)
    except OrchestratorError as exc:
        click.echo(f"FAIL consistency: {exc}", err=True)
        raise SystemExit(1)
