"""Top-level click group for the `alp` CLI."""

from __future__ import annotations

import click

from alp_cli import __version__
from alp_cli.build import build_cmd
from alp_cli.clean import clean_cmd
from alp_cli.doctor import doctor_cmd
from alp_cli.emit import emit_cmd
from alp_cli.explain import explain_cmd
from alp_cli.faultdecode import faultdecode_cmd
from alp_cli.flash import flash_cmd
from alp_cli.image import image_cmd
from alp_cli.init import init_cmd
from alp_cli.model import model_group
from alp_cli.monitor import monitor_cmd
from alp_cli.new_som import new_som_cmd
from alp_cli.renode import renode_cmd
from alp_cli.run import run_cmd
from alp_cli.size import size_cmd
from alp_cli.validate import validate_cmd


@click.group(help="Alp SDK command-line interface.")
@click.version_option(__version__, prog_name="alp")
def cli() -> None:
    """Alp SDK command-line interface."""


cli.add_command(build_cmd)
cli.add_command(clean_cmd)
cli.add_command(doctor_cmd)
cli.add_command(emit_cmd)
cli.add_command(explain_cmd)
cli.add_command(faultdecode_cmd)
cli.add_command(flash_cmd)
cli.add_command(image_cmd)
cli.add_command(init_cmd)
cli.add_command(model_group)
cli.add_command(monitor_cmd)
cli.add_command(new_som_cmd)
cli.add_command(renode_cmd)
cli.add_command(run_cmd)
cli.add_command(size_cmd)
cli.add_command(validate_cmd)


if __name__ == "__main__":
    cli()
