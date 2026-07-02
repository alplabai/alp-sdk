"""`alp flash` -- program the built project onto attached hardware.

Thin wrapper over the SAME dispatcher `west alp-flash` drives:
`scripts/west_commands/alp_flash.py` walks `build/system-manifest.yaml`
(produced by `alp build` / `west alp-build`) and hands every slice +
helper MCU to its registered backend in `scripts/flash_backends/`,
honouring the manifest's `boot_order:`.  No flashing logic lives here.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import python_exe, sdk_root, subprocess_env
from alp_cli.build import resolve_app


@click.command(name="flash",
               help="Flash every built slice + helper MCU (like west alp-flash).")
@click.argument("app_path", required=False, default=None)
@click.option("--core", default=None,
              help="Flash only the slice with this core ID.")
@click.option("--helper", default=None,
              help="Flash only the helper MCU with this name.")
@click.option("--dry-run", is_flag=True,
              help="Print the flash commands but don't run them.")
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
@click.option("--skip-missing-tools", is_flag=True,
              help="Warn + skip slices whose flash tool is not on PATH.")
def flash_cmd(app_path: str | None, core: str | None, helper: str | None,
              dry_run: bool, build_root: Path | None,
              skip_missing_tools: bool) -> None:
    project = resolve_app(app_path, prog="alp flash")
    root = sdk_root()
    if root is None:
        click.echo(
            "alp flash: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    cmd = [python_exe(),
           str(root / "scripts" / "west_commands" / "alp_flash.py"),
           str(project)]
    if core:
        cmd += ["--core", core]
    if helper:
        cmd += ["--helper", helper]
    if dry_run:
        cmd += ["--dry-run"]
    if build_root:
        cmd += ["--build-root", str(build_root)]
    if skip_missing_tools:
        cmd += ["--skip-missing-tools"]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
