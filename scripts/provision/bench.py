# SPDX-License-Identifier: Apache-2.0
"""Bench abstraction for SoM provisioning.

Console (serial | raw TCP), Power (SCPI | labgrid | manual), Probe (J-Link by
serial number | bench wrapper script), Operator prompts, and ``load_bench()``
for the PRIVATE ``bench.yaml``. Nothing here names a bench: every address,
port and serial number comes from that file.

``Console.expect()`` is the one expect loop in the package. Everything that
waits for console input -- U-Boot prompts, Flash Writer dialogs, even the
XMODEM ACK/NAK bytes -- goes through it.

See docs/provisioning-v2n.md.
"""

from __future__ import annotations

import codecs
import re
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import yaml

_TAIL = 4096


class BenchError(Exception):
    """A hardware or transport failure on the bench."""


class ExpectTimeout(BenchError):
    def __init__(self, pattern: str, tail: str, timeout: float) -> None:
        self.pattern = pattern
        self.tail = tail
        super().__init__(
            f"no match for {pattern!r} within {timeout:g}s; last console text:\n{tail}"
        )


# --------------------------------------------------------------------------
# Console
# --------------------------------------------------------------------------


class Console:
    """Base class. Transports implement _read_raw/_write_raw/close only."""

    def __init__(self) -> None:
        self.transcript: list[str] = []
        self._buf = ""
        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")

    # -- transport hooks ---------------------------------------------------
    def _read_raw(self, timeout: float) -> bytes:  # b"" on timeout
        raise NotImplementedError

    def _write_raw(self, data: bytes) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass

    # -- shared behaviour --------------------------------------------------
    def write(self, data: bytes) -> None:
        self._write_raw(data)

    def send_line(self, line: str, eol: str = "\r") -> None:
        self._write_raw((line + eol).encode())

    def _pull(self, timeout: float) -> bool:
        data = self._read_raw(timeout)
        if not data:
            return False
        text = self._decoder.decode(data)
        if text:
            self.transcript.append(text)
            self._buf += text
        return True

    def expect(self, pattern: str | re.Pattern[str], timeout: float) -> re.Match[str]:
        """Wait until `pattern` matches the unconsumed console text.

        On a match the buffer is consumed through match.end() and the match is
        returned; ``match.string[:match.start()]`` is the text that preceded it.
        """
        return self.expect_any({"": pattern}, timeout)[1]

    def expect_any(
        self, patterns: dict[str, str | re.Pattern[str]], timeout: float
    ) -> tuple[str, re.Match[str]]:
        """Like expect(); returns (key, match) of the EARLIEST match in the buffer."""
        compiled = {
            k: re.compile(p) if isinstance(p, str) else p for k, p in patterns.items()
        }
        deadline = time.monotonic() + timeout
        # ponytail: re-searches the whole unconsumed buffer after each read
        # (quadratic on a long boot log); fine for console-sized text.
        while True:
            best: tuple[str, re.Match[str]] | None = None
            for key, rx in compiled.items():
                m = rx.search(self._buf)
                if m and (best is None or m.start() < best[1].start()):
                    best = (key, m)
            if best:
                self._buf = self._buf[best[1].end() :]
                return best
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                shown = " | ".join(rx.pattern for rx in compiled.values())
                raise ExpectTimeout(shown, self._buf[-_TAIL:], timeout)
            self._pull(min(remaining, 0.5))

    def drain(self, quiet_s: float = 0.2, max_s: float = 10.0) -> str:
        """Read until the console is quiet for `quiet_s`; return and consume
        everything unconsumed. `max_s` bounds a console that never goes quiet."""
        deadline = time.monotonic() + max_s
        while time.monotonic() < deadline and self._pull(quiet_s):
            pass
        text, self._buf = self._buf, ""
        return text


class SerialConsole(Console):
    """pyserial port, opened lazily on first I/O."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        super().__init__()
        self.port, self.baud = port, baud
        self._ser = None

    def _open(self):
        if self._ser is None:
            import serial  # lazy: only a real bench needs pyserial

            try:
                self._ser = serial.Serial(self.port, self.baud, timeout=0)
            except serial.SerialException as e:
                raise BenchError(f"cannot open console {self.port}: {e}") from e
        return self._ser

    def _read_raw(self, timeout: float) -> bytes:
        s = self._open()
        s.timeout = timeout
        data = s.read(1)
        if data and s.in_waiting:
            data += s.read(s.in_waiting)
        return data

    def _write_raw(self, data: bytes) -> None:
        s = self._open()
        s.write(data)
        s.flush()

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None


class TcpConsole(Console):
    """Raw TCP console (ser2net-style), connected lazily on first I/O."""

    def __init__(self, host: str, port: int) -> None:
        super().__init__()
        self.host, self.port = host, port
        self._sock: socket.socket | None = None

    def _open(self) -> socket.socket:
        if self._sock is None:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=5)
            except OSError as e:
                raise BenchError(
                    f"cannot connect console {self.host}:{self.port}: {e}"
                ) from e
        return self._sock

    def _read_raw(self, timeout: float) -> bytes:
        s = self._open()
        s.settimeout(timeout)
        try:
            data = s.recv(4096)
        except TimeoutError:
            return b""
        if not data:
            raise BenchError(f"console {self.host}:{self.port} closed the connection")
        return data

    def _write_raw(self, data: bytes) -> None:
        self._open().sendall(data)

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None


# --------------------------------------------------------------------------
# Operator
# --------------------------------------------------------------------------


class Operator:
    def __init__(self, ask=input, say=print) -> None:
        self._ask = ask
        self._say = say

    def confirm(self, message: str) -> None:
        """Wait for Enter; typing 'abort' raises BenchError."""
        self._say(message)
        if (
            self._ask("[Enter] to continue, 'abort' to stop: ").strip().lower()
            == "abort"
        ):
            raise BenchError(f"operator aborted at: {message}")

    def ask(self, message: str) -> str:
        return self._ask(f"{message} ").strip()


# --------------------------------------------------------------------------
# Power
# --------------------------------------------------------------------------


class Power:
    def on(self) -> None:
        raise NotImplementedError

    def off(self) -> None:
        raise NotImplementedError

    def cycle(self, off_s: float = 3.0) -> None:
        self.off()
        time.sleep(off_s)
        self.on()

    def is_on(self) -> bool | None:  # None = unknowable
        return None


class ScpiPower(Power):
    """SCPI over raw TCP, one connection per command.

    Command syntax is the bench-proven ``OUTP CH<n>,ON|OFF`` /
    ``OUTP? CH<n>`` form, not the IEEE ``(@n)`` channel-list form.
    """

    def __init__(
        self, host: str, port: int, channel: int, connect=socket.create_connection
    ) -> None:
        self.host, self.port, self.channel = host, port, int(channel)
        self._connect = connect

    def _send(self, cmd: str, reply: bool = False) -> str:
        try:
            with self._connect((self.host, self.port), timeout=3) as s:
                s.sendall((cmd + "\n").encode())
                if not reply:
                    time.sleep(0.2)  # let the PSU act before the socket closes
                    return ""
                data = b""
                while not data.endswith(b"\n"):
                    chunk = s.recv(256)
                    if not chunk:
                        break
                    data += chunk
                return data.decode(errors="replace").strip()
        except OSError as e:
            raise BenchError(f"SCPI {self.host}:{self.port} {cmd!r}: {e}") from e

    def on(self) -> None:
        self._send(f"OUTP CH{self.channel},ON")

    def off(self) -> None:
        self._send(f"OUTP CH{self.channel},OFF")

    def is_on(self) -> bool | None:
        r = self._send(f"OUTP? CH{self.channel}", reply=True).upper()
        return {"ON": True, "1": True, "OFF": False, "0": False}.get(r)


class LabgridPower(Power):
    """labgrid-client power control; the place must already be acquired."""

    def __init__(
        self, place: str, runner=subprocess.run, exe: str = "labgrid-client"
    ) -> None:
        self.place = place
        self._runner = runner
        self._exe = exe

    def _lg(self, action: str) -> str:
        argv = [self._exe, "-p", self.place, "power", action]
        try:
            r = self._runner(argv, capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise BenchError(f"{' '.join(argv)}: {e}") from e
        if r.returncode != 0:
            raise BenchError(
                f"{' '.join(argv)} failed ({r.returncode}): {(r.stderr or r.stdout).strip()}"
            )
        return r.stdout

    def on(self) -> None:
        self._lg("on")

    def off(self) -> None:
        self._lg("off")

    def is_on(self) -> bool | None:
        words = re.findall(r"\b(on|off)\b", self._lg("get").lower())
        return (words[-1] == "on") if words else None


class ManualPower(Power):
    def __init__(self, operator: Operator) -> None:
        self.operator = operator

    def on(self) -> None:
        self.operator.confirm("Switch the unit's power ON.")

    def off(self) -> None:
        self.operator.confirm("Switch the unit's power OFF.")

    def cycle(self, off_s: float = 3.0) -> None:
        self.operator.confirm(
            f"Power-cycle the unit: OFF, wait at least {off_s:g} s, then ON."
        )


# --------------------------------------------------------------------------
# Probe
# --------------------------------------------------------------------------

_DPID_RE = re.compile(r"(?:SW-DP with ID|DPIDR:?)\s*0x([0-9A-Fa-f]{8})")


def _check_path(path: Path) -> str:
    s = str(path)
    if re.search(r"[\s,]", s):
        raise ValueError(f"probe file path must not contain whitespace or commas: {s}")
    return s


class Probe:
    def dp_id(self) -> int:  # fresh session
        raise NotImplementedError

    def loadbin(self, path: Path, addr: int) -> None:  # one session, then exit
        raise NotImplementedError

    def savebin(
        self, path: Path, addr: int, size: int
    ) -> None:  # ALWAYS a fresh session
        raise NotImplementedError

    def reset_run(self) -> None:
        raise NotImplementedError


class JLinkProbe(Probe):
    """J-Link Commander, one process (= one probe session) per call.

    Every command file starts with ``exec DisableAutoUpdateFW`` BEFORE
    ``connect``: Commander opens the probe lazily, and a firmware-update write
    to a cloned probe is unrecoverable, so the guard must precede the open
    (no ``-autoconnect``).
    """

    def __init__(
        self,
        serial_no: str,
        exe: str = "JLinkExe",
        device: str = "GD32G553MEY7TR",
        speed_khz: int = 4000,
        runner=subprocess.run,
    ) -> None:
        self.serial_no, self.exe, self.device, self.speed_khz = (
            str(serial_no),
            exe,
            device,
            speed_khz,
        )
        self._runner = runner

    def _session(self, commands: list[str], check: bool = True) -> str:
        script = (
            "\n".join(["exec DisableAutoUpdateFW", "connect", *commands, "exit"]) + "\n"
        )
        with tempfile.TemporaryDirectory() as td:
            cmdfile = Path(td) / "cmd.jlink"
            cmdfile.write_text(script, encoding="utf-8", newline="\n")
            argv = [
                self.exe, "-NoGui", "1", "-ExitOnError", "1",
                "-SelectEmuBySN", self.serial_no, "-device", self.device,
                "-if", "SWD", "-speed", str(self.speed_khz),
                "-CommanderScript", str(cmdfile),
            ]  # fmt: skip
            try:
                r = self._runner(argv, capture_output=True, text=True, timeout=300)
            except (OSError, subprocess.TimeoutExpired) as e:
                raise BenchError(f"J-Link {self.serial_no}: {e}") from e
        out = (r.stdout or "") + (r.stderr or "")
        if check and (
            r.returncode != 0
            or re.search(r"(?i)\berror\b|cannot connect|could not|failed", out)
        ):
            raise BenchError(
                f"J-Link {self.serial_no} session failed (rc {r.returncode}):\n{out[-_TAIL:]}"
            )
        return out

    def dp_id(self) -> int:
        # The DP answers before the device-specific connect can fail, so a
        # wrong target still yields its ID for the caller's gate to refuse.
        out = self._session([], check=False)
        m = _DPID_RE.search(out)
        if not m:
            raise BenchError(
                f"J-Link {self.serial_no}: no DP ID in output:\n{out[-_TAIL:]}"
            )
        return int(m.group(1), 16)

    def loadbin(self, path: Path, addr: int) -> None:
        self._session(["r", "h", f"loadbin {_check_path(path)},{addr:#x}"])

    def savebin(self, path: Path, addr: int, size: int) -> None:
        path = Path(path)
        path.unlink(missing_ok=True)
        self._session(["h", f"savebin {_check_path(path)},{addr:#x},{size:#x}"])
        if not path.is_file() or path.stat().st_size != size:
            raise BenchError(f"J-Link savebin produced no {size}-byte file at {path}")

    def reset_run(self) -> None:
        self._session(["r", "g"])


class ScriptProbe(Probe):
    """The bench's probe wrapper script. Interface (subcommands):
    ``dp-id`` (prints 0x........), ``loadbin PATH ADDR``,
    ``savebin PATH ADDR SIZE``, ``reset-run``; exit 0 on success."""

    def __init__(self, wrapper: Path, runner=subprocess.run) -> None:
        self.wrapper = Path(wrapper)
        self._runner = runner

    def _call(self, *args: str) -> str:
        argv = (["bash"] if self.wrapper.suffix == ".sh" else []) + [
            str(self.wrapper),
            *args,
        ]
        try:
            r = self._runner(argv, capture_output=True, text=True, timeout=300)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise BenchError(f"{' '.join(argv)}: {e}") from e
        if r.returncode != 0:
            raise BenchError(
                f"{' '.join(argv)} failed ({r.returncode}): {(r.stderr or r.stdout)[-_TAIL:]}"
            )
        return r.stdout

    def dp_id(self) -> int:
        m = re.search(r"0x([0-9A-Fa-f]{8})", self._call("dp-id"))
        if not m:
            raise BenchError("probe wrapper dp-id printed no 0x........ value")
        return int(m.group(1), 16)

    def loadbin(self, path: Path, addr: int) -> None:
        self._call("loadbin", str(path), f"{addr:#x}")

    def savebin(self, path: Path, addr: int, size: int) -> None:
        self._call("savebin", str(path), f"{addr:#x}", f"{size:#x}")
        if not Path(path).is_file() or Path(path).stat().st_size != size:
            raise BenchError(
                f"probe wrapper savebin produced no {size}-byte file at {path}"
            )

    def reset_run(self) -> None:
        self._call("reset-run")


# --------------------------------------------------------------------------
# bench.yaml
# --------------------------------------------------------------------------


@dataclass
class Bench:
    console: Console
    power: Power
    probe: Probe | None
    operator: Operator
    linux_user: str
    linux_host: str | None
    i2c_bus: dict[str, int]  # values may be None while TBD
    scif: dict  # {"flash_writer": Path, "baud": int, "program_start": {"bl2_mmc": int|None, "fip": int|None}}
    raw: dict


def _need(d: dict, key: str, where: str):
    if not isinstance(d, dict) or key not in d:
        raise ValueError(f"bench.yaml: missing '{where}{key}'")
    return d[key]


def _int_or_none(v, where: str) -> int | None:
    if v is None or (isinstance(v, int) and not isinstance(v, bool)):
        return v
    raise ValueError(f"bench.yaml: '{where}' must be an integer or null, got {v!r}")


def load_bench(path: Path, operator: Operator | None = None) -> Bench:
    """Parse and validate the PRIVATE bench.yaml. Nothing is opened here:
    consoles connect on first I/O. Relative paths resolve against the file's
    directory. ``null`` is accepted for bench-TBD integers (program_start,
    i2c bus numbers); the consumer must refuse to use a None."""
    path = Path(path)
    raw = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    base = path.parent
    operator = operator or Operator()

    def rel(p) -> Path:
        p = Path(p)
        return p if p.is_absolute() else base / p

    c = _need(raw, "console", "")
    kind = _need(c, "kind", "console.")
    if kind == "serial":
        console: Console = SerialConsole(
            str(_need(c, "port", "console.")), int(c.get("baud", 115200))
        )
    elif kind == "tcp":
        console = TcpConsole(
            str(_need(c, "host", "console.")), int(_need(c, "port", "console."))
        )
    else:
        raise ValueError(f"bench.yaml: console.kind must be serial|tcp, got {kind!r}")

    p = _need(raw, "power", "")
    kind = _need(p, "kind", "power.")
    if kind == "scpi":
        power: Power = ScpiPower(
            str(_need(p, "host", "power.")),
            int(_need(p, "port", "power.")),
            int(_need(p, "channel", "power.")),
        )
    elif kind == "labgrid":
        power = LabgridPower(str(_need(p, "place", "power.")))
    elif kind == "manual":
        power = ManualPower(operator)
    else:
        raise ValueError(
            f"bench.yaml: power.kind must be scpi|labgrid|manual, got {kind!r}"
        )

    probe: Probe | None = None
    pr = raw.get("probe")
    if pr:
        kind = _need(pr, "kind", "probe.")
        if kind == "jlink":
            extra = {k: pr[k] for k in ("exe", "device", "speed_khz") if k in pr}
            probe = JLinkProbe(str(_need(pr, "serial", "probe.")), **extra)
        elif kind == "script":
            probe = ScriptProbe(rel(_need(pr, "wrapper", "probe.")))
        else:
            raise ValueError(
                f"bench.yaml: probe.kind must be jlink|script, got {kind!r}"
            )

    linux = raw.get("linux") or {}

    ib = _need(raw, "i2c_bus", "")
    i2c_bus = {
        k: _int_or_none(_need(ib, k, "i2c_bus."), f"i2c_bus.{k}")
        for k in ("eeprom", "pmic", "brd")
    }

    s = _need(raw, "scif", "")
    ps = _need(s, "program_start", "scif.")
    scif = {
        "flash_writer": rel(_need(s, "flash_writer", "scif.")),
        "baud": int(_need(s, "baud", "scif.")),
        "program_start": {
            k: _int_or_none(
                _need(ps, k, "scif.program_start."), f"scif.program_start.{k}"
            )
            for k in ("bl2_mmc", "fip")
        },
    }

    return Bench(
        console=console,
        power=power,
        probe=probe,
        operator=operator,
        linux_user=str(linux.get("user") or "root"),
        linux_host=linux.get("host"),
        i2c_bus=i2c_bus,
        scif=scif,
        raw=raw,
    )
