"""`alp monitor` -- open a serial console to the attached board.

A thin front door over pyserial's miniterm.  Port comes from ``--port``;
baud from ``--baud`` (default 115200, the SDK-wide console default).

There is no safe cross-platform guess for the port itself (COMx vs
/dev/ttyUSBx vs /dev/cu.*), so when no port is given -- or the
requested one does not exist -- the command lists every serial port
pyserial can see and exits non-zero instead of hanging on a wrong
device.

Board-context port resolution (a ``console:`` block in the project's
``system-manifest.yaml``) is deliberately NOT implemented yet: the
board schema and orchestrator do not emit one today.  Add it there
first, then teach this verb to read it.
"""

from __future__ import annotations

import subprocess

import click

from alp_cli._workspace import python_exe

DEFAULT_BAUD = 115200


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
@click.option("--baud", default=DEFAULT_BAUD, type=int, show_default=True,
              help="Baud rate.")
def monitor_cmd(port: str | None, baud: int) -> None:
    try:
        import serial  # noqa: F401  (validates pyserial is installed)
    except ImportError:
        click.echo(
            "alp monitor: pyserial is required.  Install via `pip install pyserial`.",
            err=True,
        )
        raise SystemExit(1)

    if port is None:
        _die_listing_ports("no --port given")
    if port not in {device for device, _ in _available_ports()}:
        _die_listing_ports(f"port '{port}' not found")

    click.echo(f"alp monitor: {port} @ {baud} (Ctrl+] to quit)")
    rc = subprocess.run(
        [python_exe(), "-m", "serial.tools.miniterm", port, str(baud)]
    ).returncode
    if rc != 0:
        raise SystemExit(rc)
