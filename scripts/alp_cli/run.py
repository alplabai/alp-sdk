"""`alp run` -- build the current project + run it on native_sim.

Reuses the shared build path from `alp_cli.build` (the same single-image
helper `alp build --board ...` uses), then executes the produced binary.
For heterogeneous multi-core projects, build with `alp build` and program
hardware with `alp flash` instead -- native_sim is a single-image target.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli.build import build_single_image, resolve_app


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
