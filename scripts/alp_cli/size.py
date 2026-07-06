"""`alp size` -- per-slice flash/RAM footprint vs the SoM memory budget.

Thin wrapper over the SAME reporter `west alp-size` drives:
`scripts/west_commands/alp_size.py` walks `build/system-manifest.yaml`
(produced by `alp build` / `west alp-build`), measures every Zephyr
slice's `zephyr.elf`, and compares FLASH/RAM usage against the budget
resolved from the SoM's SoC metadata.  No measuring logic lives here.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import python_exe, sdk_root, subprocess_env
from alp_cli.build import resolve_app


@click.command(name="size",
               help="Report per-slice flash/RAM footprint vs the SoM "
                    "budget (like west alp-size).")
@click.argument("app_path", required=False, default=None)
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
@click.option("--board", default=None,
              help="Override the SoM SKU used to resolve the memory budget "
                   "(default: hw_info.sku from the manifest).")
@click.option("--json", "json_out", is_flag=True,
              help="Emit the machine-readable report (like `alp doctor "
                   "--json`) instead of the human table.")
@click.option("--fail-over-budget", is_flag=True,
              help="Exit non-zero if any slice exceeds its resolved budget "
                   "(slices with an unknown budget are skipped + reported).")
def size_cmd(app_path: str | None, build_root: Path | None,
             board: str | None, json_out: bool,
             fail_over_budget: bool) -> None:
    project = resolve_app(app_path, prog="alp size")
    root = sdk_root()
    if root is None:
        click.echo(
            "alp size: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    cmd = [python_exe(),
           str(root / "scripts" / "west_commands" / "alp_size.py"),
           str(project)]
    if build_root:
        cmd += ["--build-root", str(build_root)]
    if board:
        cmd += ["--board", board]
    if json_out:
        cmd += ["--json"]
    if fail_over_budget:
        cmd += ["--fail-over-budget"]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
