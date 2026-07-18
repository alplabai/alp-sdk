"""`alp run` -- build the current project + run it on native_sim.

Plain `west build` single-image path (ADR-0020 Phase 4 preview: the
fan-out `alp build`/`alp flash` commands were retired -- this is now the
only in-repo build front door), then executes the produced binary.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import click

from alp_cli._workspace import find_project


def resolve_app(app_path: str | None, prog: str = "alp run") -> Path:
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
    """Plain `west build -b <board>` (single-image path)."""
    if shutil.which("west") is None:
        click.echo(
            "alp run: `west` not found on PATH -- run scripts/bootstrap.sh "
            "(or bootstrap.ps1) and activate the workspace venv",
            err=True,
        )
        return 1
    build = build_dir or project_dir / "build" / board.replace("/", "_")
    build.mkdir(parents=True, exist_ok=True)
    return subprocess.run(
        ["west", "build", "-b", board, "-d", str(build), str(project_dir)]
    ).returncode


def _build_and_exec_native_sim(project_dir: Path) -> int:
    build = project_dir / "build" / "native_sim"
    rc = build_single_image(project_dir, "native_sim", build_dir=build)
    if rc != 0:
        return rc
    exe = build / "zephyr" / "zephyr.exe"
    if not exe.is_file():
        click.echo(f"alp run: built binary not found at {exe}", err=True)
        return 1
    return subprocess.run([str(exe)]).returncode


def _build_for_board(project_dir: Path, board: str, flash: bool) -> int:
    build = project_dir / "build" / board.replace("/", "_")
    rc = build_single_image(project_dir, board, build_dir=build)
    if rc != 0:
        return rc
    if flash:
        return subprocess.run(["west", "flash", "-d", str(build)]).returncode
    return 0


@click.command(name="run", help="Build and run the current project on native_sim.")
@click.option("--board", default=None, help="Real-hardware build (skips native_sim).")
@click.option("--flash", is_flag=True, help="With --board: flash after build.")
def run_cmd(board: str | None, flash: bool) -> None:
    project = resolve_app(None, prog="alp run")
    if board:
        rc = _build_for_board(project, board, flash)
    else:
        rc = _build_and_exec_native_sim(project)
    if rc != 0:
        raise SystemExit(rc)
