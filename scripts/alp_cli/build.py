"""`alp build` -- build the current project (multi-core aware).

Two paths, one front door:

  * **Orchestrated fan-out (default).**  Delegates to the SAME pipeline as
    `west alp-build`: pre-flight the board.yaml through
    `scripts/validate_board_yaml.py`, then fan out every non-`off` core via
    `python -m alp_orchestrate` (slices + `build/system-manifest.yaml`).
    This is the path that understands heterogeneous board.yaml projects
    (Zephyr + Yocto + baremetal slices).

  * **Single-image fallback (`--board <zephyr-board>`).**  A plain
    `west build -b <board>` into `build/<board>/` -- the pre-facade
    `alp run` behaviour, still the right tool for a one-core Zephyr app
    when you want to name the Zephyr board target directly.

Both paths are thin wrappers: no orchestrator logic lives here.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import click

from alp_cli._workspace import (
    find_project,
    find_west_topdir,
    python_exe,
    sdk_root,
    subprocess_env,
)


def resolve_app(app_path: str | None, prog: str = "alp build") -> Path:
    """Resolve the app directory (argument, else walk up from cwd)."""
    if app_path:
        project = Path(app_path).resolve()
        if not (project / "board.yaml").is_file():
            click.echo(f"{prog}: no board.yaml in {project}", err=True)
            raise SystemExit(1)
        return project
    project = find_project(Path.cwd())
    if project is None:
        click.echo(
            f"{prog}: no board.yaml found in this directory or any parent",
            err=True,
        )
        raise SystemExit(1)
    return project


def build_single_image(project_dir: Path, board: str,
                       build_dir: Path | None = None) -> int:
    """Plain `west build -b <board>` (single-image path, shared with run)."""
    if shutil.which("west") is None:
        click.echo(
            "alp build: `west` not found on PATH -- run scripts/bootstrap.sh "
            "(or bootstrap.ps1) and activate the workspace venv",
            err=True,
        )
        return 1
    build = build_dir or project_dir / "build" / board.replace("/", "_")
    build.mkdir(parents=True, exist_ok=True)
    return subprocess.run(
        ["west", "build", "-b", board, "-d", str(build), str(project_dir)]
    ).returncode


def build_fanout(project_dir: Path, *, core: str | None = None,
                 no_parallel: bool = False, no_validate: bool = False,
                 build_root: Path | None = None) -> int:
    """The `west alp-build` pipeline: validate, then orchestrator fan-out."""
    root = sdk_root()
    if root is None:
        click.echo(
            "alp build: cannot locate the alp-sdk checkout "
            "(set ALP_SDK_ROOT or install the CLI editable from the SDK)",
            err=True,
        )
        return 1
    if find_west_topdir(project_dir) is None and find_west_topdir(root) is None:
        click.echo(
            "alp build: not inside a west workspace -- Zephyr slices need one. "
            "Run scripts/bootstrap.sh (or bootstrap.ps1) first, or use "
            "`alp build --board <target>` for a plain single-image build.",
            err=True,
        )
        return 1

    board_yaml = project_dir / "board.yaml"
    build = (build_root or project_dir / "build").resolve()
    python = python_exe()
    env = subprocess_env(root)

    if not no_validate:
        rc = subprocess.run(
            [python, str(root / "scripts" / "validate_board_yaml.py"),
             "--input", str(board_yaml)],
            env=env,
        ).returncode
        if rc != 0:
            click.echo(f"alp build: board.yaml validation failed (rc={rc})",
                       err=True)
            return rc

    cmd = [python, "-m", "alp_orchestrate",
           "--input", str(board_yaml), "--build-root", str(build)]
    if core:
        cmd += ["--core", core]
    if no_parallel:
        cmd += ["--no-parallel"]
    click.echo(f"alp build: $ {' '.join(cmd)}")
    return subprocess.run(cmd, env=env, cwd=str(project_dir)).returncode


@click.command(name="build",
               help="Build the project (multi-core fan-out, like west alp-build).")
@click.argument("app_path", required=False, default=None)
@click.option("--core", default=None,
              help="Limit the fan-out to one core ID.")
@click.option("--no-parallel", is_flag=True,
              help="Force sequential slice dispatch.")
@click.option("--no-validate", is_flag=True,
              help="Skip the board.yaml pre-flight validation.")
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Override the build root (default: <app>/build).")
@click.option("--board", default=None,
              help="Single-image fallback: plain `west build -b <board>` "
                   "(skips the orchestrator).")
def build_cmd(app_path: str | None, core: str | None, no_parallel: bool,
              no_validate: bool, build_root: Path | None,
              board: str | None) -> None:
    project = resolve_app(app_path)
    if board:
        rc = build_single_image(project, board)
    else:
        rc = build_fanout(project, core=core, no_parallel=no_parallel,
                          no_validate=no_validate, build_root=build_root)
    if rc != 0:
        raise SystemExit(rc)
