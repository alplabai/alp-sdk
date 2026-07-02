"""`alp monitor` -- open a serial console to the attached board.

A thin front door over pyserial's miniterm.  Port/baud resolution order:

  1. ``--port`` / ``--baud`` flags.
  2. A ``console:`` block (``port:`` / ``baud:``) in the nearest project's
     ``build/system-manifest.yaml`` or ``board.yaml``, when one declares it.
  3. Baud falls back to 115200 (the SDK-wide console default).

There is no safe cross-platform guess for the port itself (COMx vs
/dev/ttyUSBx vs /dev/cu.*), so when no port can be resolved -- or the
requested one does not exist -- the command lists every serial port
pyserial can see and exits non-zero instead of hanging on a wrong device.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any

import click

from alp_cli._workspace import find_project, python_exe

DEFAULT_BAUD = 115200


def _console_context(start: Path) -> dict[str, Any]:
    """Best-effort console hints from the nearest project.

    Looks for a ``console:`` mapping in ``build/system-manifest.yaml`` first
    (the orchestrator's single source of truth), then ``board.yaml``.
    Returns an empty dict when neither declares one -- most projects today
    don't, and the flags/defaults then apply.
    """
    project = find_project(start)
    if project is None:
        return {}
    try:
        import yaml
    except ImportError:  # pragma: no cover - PyYAML is a hard dependency
        return {}
    for candidate in (project / "build" / "system-manifest.yaml",
                      project / "board.yaml"):
        if not candidate.is_file():
            continue
        try:
            doc = yaml.safe_load(candidate.read_text(encoding="utf-8"))
        except (OSError, yaml.YAMLError):
            continue
        console = (doc or {}).get("console") if isinstance(doc, dict) else None
        if isinstance(console, dict):
            return console
    return {}


def _available_ports() -> list[tuple[str, str]]:
    """[(device, description)] for every serial port pyserial can see."""
    from serial.tools import list_ports

    return [(p.device, p.description or "") for p in list_ports.comports()]


def _die_listing_ports(reason: str) -> None:
    click.echo(f"alp monitor: {reason}", err=True)
    ports = _available_ports()
    if ports:
        click.echo("available serial ports:", err=True)
        for device, description in ports:
            click.echo(f"  {device}  {description}".rstrip(), err=True)
    else:
        click.echo("no serial ports detected on this host.", err=True)
    raise SystemExit(1)


@click.command(name="monitor", help="Open a serial console to the board.")
@click.option("--port", default=None,
              help="Serial port (COM7, /dev/ttyUSB0, /dev/cu.usbmodem...).")
@click.option("--baud", default=None, type=int,
              help=f"Baud rate (default: board context, else {DEFAULT_BAUD}).")
def monitor_cmd(port: str | None, baud: int | None) -> None:
    try:
        import serial  # noqa: F401  (validates pyserial is installed)
    except ImportError:
        click.echo(
            "alp monitor: pyserial is required.  Install via `pip install pyserial`.",
            err=True,
        )
        raise SystemExit(1)

    context = _console_context(Path.cwd())
    if port is None:
        ctx_port = context.get("port")
        port = str(ctx_port) if ctx_port else None
    if baud is None:
        ctx_baud = context.get("baud")
        baud = int(ctx_baud) if ctx_baud else DEFAULT_BAUD

    if port is None:
        _die_listing_ports("no --port given and the board context declares no console port")
    if port not in {device for device, _ in _available_ports()}:
        _die_listing_ports(f"port '{port}' not found")

    click.echo(f"alp monitor: {port} @ {baud} (Ctrl+] to quit)")
    rc = subprocess.run(
        [python_exe(), "-m", "serial.tools.miniterm", port, str(baud)]
    ).returncode
    if rc != 0:
        raise SystemExit(rc)
