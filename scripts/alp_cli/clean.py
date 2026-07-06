"""`alp clean` -- remove the project build directory + orchestrator cache.

Thin wrapper over the SAME cleaner `west alp-clean` drives:
`scripts/west_commands/alp_clean.py` removes `<app>/build/` and the
orchestrator's `.alp-build-state.json`.  Single-purpose; the per-slice
build dirs are self-contained so removing them is enough -- no Zephyr
or Yocto-specific cleaners are invoked, and no removal logic lives here.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import python_exe, sdk_root, subprocess_env
from alp_cli.build import resolve_app


@click.command(name="clean",
               help="Remove the build dir + orchestrator cache "
                    "(like west alp-clean).")
@click.argument("app_path", required=False, default=None)
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
@click.option("--dry-run", is_flag=True,
              help="List paths that would be removed, don't delete.")
def clean_cmd(app_path: str | None, build_root: Path | None,
              dry_run: bool) -> None:
    project = resolve_app(app_path, prog="alp clean")
    root = sdk_root()
    if root is None:
        click.echo(
            "alp clean: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    cmd = [python_exe(),
           str(root / "scripts" / "west_commands" / "alp_clean.py"),
           str(project)]
    if build_root:
        cmd += ["--build-root", str(build_root)]
    if dry_run:
        cmd += ["--dry-run"]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
