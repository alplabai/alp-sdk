"""`alp emit` -- print one generated config artefact from board.yaml.

The single emit front door: a superset of both `scripts/alp_project.py
--emit` (project/per-core artefacts) and `west alp-emit` (the
orchestrator's ADR-0014 artefacts).  Read-only: no build, nothing
written unless `--output` is given -- it just shows exactly what a
consuming tool (CMake, Yocto, the IDE) would see.

Delegation, not duplication: every mode is emitted by the ONE
implementation that owns it --

  * project/per-core modes -> `scripts/alp_project.py --emit <mode>`
    (which itself routes the v2 project-level modes through
    `scripts/alp_orchestrate/`);
  * orchestrator-only modes -> `python -m alp_orchestrate --emit <mode>`,
    the exact invocation `west alp-emit` performs.

So `alp emit`, `west alp-emit`, and `alp_project.py` can never emit
different artefacts for the same mode.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import click

from alp_cli._workspace import find_project, python_exe, sdk_root, subprocess_env

# Mirror scripts/alp_project.py's --emit choices.  alp_project.py validates
# again, so drift surfaces as its error -- never a silent wrong emit.
PROJECT_EMIT_MODES = [
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

# Mirror the orchestrator's --emit choices (alp_orchestrate.cli) == the
# `west alp-emit` mode set.  Same drift contract: the orchestrator
# re-validates, so a stale entry here errors loudly.
ORCHESTRATOR_EMIT_MODES = [
    "system-manifest",
    "ipc-contract-h",
    "dts-reservations",
    "dts-partitions",
    "storage-mounts-c",
    "tfm-sysbuild-conf",
    "build-plan",
]

# The full front-door mode set: alp_project.py's list plus every
# orchestrator-only mode.  Overlapping modes (system-manifest,
# ipc-contract-h, dts-reservations) keep routing via alp_project.py,
# which delegates them to the same orchestrator emitters anyway.
EMIT_MODES = PROJECT_EMIT_MODES + [
    m for m in ORCHESTRATOR_EMIT_MODES if m not in PROJECT_EMIT_MODES
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
@click.option("--build-root", default=None, type=click.Path(path_type=Path),
              help="Build root used for build-plan slice paths "
                   "(default: build).")
def emit_cmd(mode: str, input_path: Path | None, output: Path | None,
             core: str | None, build_root: Path | None) -> None:
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

    if mode in PROJECT_EMIT_MODES:
        rc = _emit_via_alp_project(root, mode, input_path, output, core,
                                   build_root)
    else:
        rc = _emit_via_orchestrator(root, mode, input_path, output, core,
                                    build_root)
    if rc != 0:
        raise SystemExit(rc)


def _emit_via_alp_project(root: Path, mode: str, input_path: Path,
                          output: Path | None, core: str | None,
                          build_root: Path | None) -> int:
    """Project/per-core modes: `scripts/alp_project.py --emit <mode>`."""
    if build_root:
        # Only build-plan (an orchestrator-only mode) consumes it.
        click.echo(f"alp emit: --build-root is ignored for {mode}", err=True)
    cmd = [python_exe(), str(root / "scripts" / "alp_project.py"),
           "--input", str(input_path), "--emit", mode]
    if output:
        cmd += ["--output", str(output)]
    if core:
        cmd += ["--core", core]
    return subprocess.run(cmd, env=subprocess_env(root)).returncode


def _emit_via_orchestrator(root: Path, mode: str, input_path: Path,
                           output: Path | None, core: str | None,
                           build_root: Path | None) -> int:
    """Orchestrator-only modes: `python -m alp_orchestrate --emit <mode>`
    -- the exact invocation `west alp-emit` performs."""
    if core:
        # Matches alp_project.py's behaviour for project-level emits.
        click.echo(f"alp emit: --core is ignored for {mode} "
                   f"(project-level emit)", err=True)
    cmd = [python_exe(), "-m", "alp_orchestrate",
           "--input", str(input_path), "--emit", mode]
    if build_root:
        cmd += ["--build-root", str(build_root)]

    if output is None:
        return subprocess.run(cmd, env=subprocess_env(root)).returncode

    # The orchestrator emits to stdout only; --output is CLI plumbing
    # (capture + write), mirroring alp_project.py's _write_or_print.
    proc = subprocess.run(cmd, env=subprocess_env(root),
                          capture_output=True, text=True)
    if proc.stderr:
        click.echo(proc.stderr, err=True, nl=False)
    if proc.returncode != 0:
        return proc.returncode
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(proc.stdout, encoding="utf-8")
    click.echo(f"alp emit: wrote {output} ({len(proc.stdout)} bytes)",
               err=True)
    return 0
