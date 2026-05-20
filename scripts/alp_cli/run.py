"""`alp run` -- build the current project + run it on native_sim."""

from __future__ import annotations

import subprocess
from pathlib import Path

import click


def _find_project(start: Path) -> Path | None:
    cursor = start.resolve()
    while True:
        if (cursor / "board.yaml").is_file():
            return cursor
        if cursor.parent == cursor:
            return None
        cursor = cursor.parent


def _build_and_exec_native_sim(project_dir: Path) -> int:
    build = project_dir / "build" / "native_sim"
    build.mkdir(parents=True, exist_ok=True)
    cmake = ["west", "build", "-b", "native_sim", "-d", str(build), str(project_dir)]
    proc = subprocess.run(cmake)
    if proc.returncode != 0:
        return proc.returncode
    exe = build / "zephyr" / "zephyr.exe"
    if not exe.is_file():
        click.echo(f"alp run: built binary not found at {exe}", err=True)
        return 1
    return subprocess.run([str(exe)]).returncode


def _build_for_board(project_dir: Path, board: str, flash: bool) -> int:
    build = project_dir / "build" / board.replace("/", "_")
    build.mkdir(parents=True, exist_ok=True)
    rc = subprocess.run(
        ["west", "build", "-b", board, "-d", str(build), str(project_dir)]
    ).returncode
    if rc != 0:
        return rc
    if flash:
        return subprocess.run(["west", "flash", "-d", str(build)]).returncode
    return 0


@click.command(name="run", help="Build and run the current project on native_sim.")
@click.option("--board", default=None, help="Real-hardware build (skips native_sim).")
@click.option("--flash", is_flag=True, help="With --board: flash after build.")
def run_cmd(board: str | None, flash: bool) -> None:
    project = _find_project(Path.cwd())
    if project is None:
        click.echo("alp run: no board.yaml found in this directory or any parent",
                   err=True)
        raise SystemExit(1)
    if board:
        rc = _build_for_board(project, board, flash)
    else:
        rc = _build_and_exec_native_sim(project)
    if isinstance(rc, int) and rc != 0:
        raise SystemExit(rc)
