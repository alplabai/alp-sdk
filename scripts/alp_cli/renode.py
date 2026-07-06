"""`alp renode` -- boot the built system manifest in Renode (no hardware).

Thin wrapper over the SAME smoke runner `west alp-renode` drives:
`scripts/west_commands/alp_renode.py` reads `build/system-manifest.yaml`
(produced by `alp build` / `west alp-build`), resolves the Zephyr
slice's `zephyr.elf`, maps the SoM family to a platform descriptor
under `metadata/renode/`, and boots it headless with a wall-clock
timeout.  No emulation logic lives here.  Requires the `renode`
binary on PATH -- the wrapped command exits non-zero with install
guidance when it's absent (never a silent pass).
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import python_exe, sdk_root, subprocess_env
from alp_cli.build import resolve_app


@click.command(name="renode",
               help="Boot the built manifest in Renode, headless "
                    "(like west alp-renode).")
@click.argument("app_path", required=False, default=None)
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
@click.option("--board", default=None,
              help="Override the SoM SKU used to pick the Renode platform "
                   "descriptor (default: hw_info.sku from the manifest).")
@click.option("--log", "log_path", default=None,
              type=click.Path(path_type=Path),
              help="Tee the Renode UART/console output to this file "
                   "(default: <build-root>/renode.log).")
@click.option("--timeout", default=None, type=int,
              help="Wall-clock cap for the Renode run, seconds "
                   "(default: the wrapped command's 120).")
@click.option("--expect", default=None,
              help="Stop early (exit 0) when this substring appears in "
                   "the console; exit 1 if the run ends without it.")
@click.option("--image-bundle", default=None,
              type=click.Path(path_type=Path),
              help="Directory of pre-built per-slice artefacts (dual-OS "
                   "boot); accepted for parity, unused by the "
                   "single-Zephyr-slice smoke.")
def renode_cmd(app_path: str | None, build_root: Path | None,
               board: str | None, log_path: Path | None,
               timeout: int | None, expect: str | None,
               image_bundle: Path | None) -> None:
    project = resolve_app(app_path, prog="alp renode")
    root = sdk_root()
    if root is None:
        click.echo(
            "alp renode: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    cmd = [python_exe(),
           str(root / "scripts" / "west_commands" / "alp_renode.py"),
           str(project)]
    if build_root:
        cmd += ["--build-root", str(build_root)]
    if board:
        cmd += ["--board", board]
    if log_path:
        cmd += ["--log", str(log_path)]
    if timeout is not None:
        cmd += ["--timeout", str(timeout)]
    if expect:
        cmd += ["--expect", expect]
    if image_bundle:
        cmd += ["--image-bundle", str(image_bundle)]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
