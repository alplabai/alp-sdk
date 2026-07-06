"""`alp image` -- assemble a single flashable bundle from the manifest.

Thin wrapper over the SAME bundler `west alp-image` drives:
`scripts/west_commands/alp_image.py` walks `build/system-manifest.yaml`
(produced by `alp build` / `west alp-build`) and assembles
`build/image-bundle/` -- per-slice tar.gz archives, helper-MCU
firmware, and `bundle-manifest.json`.  No bundling logic lives here.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import python_exe, sdk_root, subprocess_env
from alp_cli.build import resolve_app


@click.command(name="image",
               help="Assemble a flashable bundle from the built manifest "
                    "(like west alp-image).")
@click.argument("app_path", required=False, default=None)
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
def image_cmd(app_path: str | None, build_root: Path | None) -> None:
    project = resolve_app(app_path, prog="alp image")
    root = sdk_root()
    if root is None:
        click.echo(
            "alp image: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        raise SystemExit(1)

    cmd = [python_exe(),
           str(root / "scripts" / "west_commands" / "alp_image.py"),
           str(project)]
    if build_root:
        cmd += ["--build-root", str(build_root)]

    rc = subprocess.run(cmd, env=subprocess_env(root)).returncode
    if rc != 0:
        raise SystemExit(rc)
