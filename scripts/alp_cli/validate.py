"""`alp validate` -- run the board.yaml validator on a file."""

from __future__ import annotations

import json
from pathlib import Path

import click

from alp_cli.diagnostic import render
from alp_cli.diagnostic_format import to_machine_json, to_sarif
from alp_cli.validator import validate_board_yaml
from alp_orchestrate import OrchestratorError, load_board_yaml


@click.command(name="validate", help="Validate a board.yaml file.")
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
                required=False, default=Path("board.yaml"))
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
@click.option("--format", "output_format", type=click.Choice(["human", "json", "sarif"]),
              default="human",
              help="human: Rust-style prose (default). json: versioned machine "
                   "document (metadata/schemas/diagnostic-v1.schema.json), "
                   "zero-based LSP ranges. sarif: SARIF 2.1.0 log, one-based "
                   "regions. json/sarif print ONLY the structured document to "
                   "stdout -- no human prose is interleaved.")
def validate_cmd(path: Path, no_color: bool, output_format: str) -> None:
    collector = validate_board_yaml(path)
    source_text = path.read_text(encoding="utf-8")
    if output_format == "human":
        for diag in collector:
            click.echo(render(diag, source_text=source_text, color=not no_color))
    elif output_format == "json":
        click.echo(json.dumps(to_machine_json(collector), indent=2))
    elif output_format == "sarif":
        click.echo(json.dumps(to_sarif(collector), indent=2))
    if collector.has_errors():
        raise SystemExit(1)
    try:
        load_board_yaml(path)
    except OrchestratorError as exc:
        click.echo(f"FAIL consistency: {exc}", err=True)
        raise SystemExit(1)
