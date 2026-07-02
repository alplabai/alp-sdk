"""`alp emit` -- print one generated config artefact from board.yaml.

Thin wrapper over `scripts/alp_project.py --emit <mode>` (which itself
routes the v2 project-level modes through `scripts/alp_orchestrate/`).
Read-only sibling of `west alp-emit`: no build, nothing written unless
`--output` is given -- it just shows exactly what a consuming tool
(CMake, Yocto, the IDE) would see.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import find_project, python_exe, sdk_root, subprocess_env

# Mirror scripts/alp_project.py's --emit choices.  alp_project.py validates
# again, so drift surfaces as its error -- never a silent wrong emit.
EMIT_MODES = [
    "zephyr-conf",
    "cmake-args",
    "yocto-conf",
    "dts-overlay",
    "hw-info-h",
    "west-libraries",
    "system-manifest",
    "dts-reservations",
    "ipc-contract-h",
    "os-topology",
    "composed-route-table",
]


@click.command(name="emit",
               help="Print a generated config artefact from board.yaml (no build).")
@click.argument("mode", type=click.Choice(EMIT_MODES))
@click.option("--input", "input_path", default=None,
              type=click.Path(path_type=Path),
              help="Path to board.yaml (default: nearest board.yaml upward).")
@click.option("--output", default=None, type=click.Path(path_type=Path),
              help="Write to this path instead of stdout.")
@click.option("--core", default=None,
              help="Limit per-core emit modes to this core ID.")
def emit_cmd(mode: str, input_path: Path | None, output: Path | None,
             core: str | None) -> None:
    root = sdk_root()
    if root is None:
        click.echo(
            "alp emit: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    if input_path is None:
        project = find_project(Path.cwd())
        if project is None:
            click.echo(
                "alp emit: no board.yaml found in this directory or any parent "
                "(or pass --input)",
                err=True,
            )
            raise SystemExit(1)
        input_path = project / "board.yaml"

    cmd = [python_exe(), str(root / "scripts" / "alp_project.py"),
           "--input", str(input_path), "--emit", mode]
    if output:
        cmd += ["--output", str(output)]
    if core:
        cmd += ["--core", core]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
