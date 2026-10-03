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
import logging
import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
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
    # A real UART RX (the board's console under a login shell) overruns on a burst:
    # bytes get corrupted (spaces turned into '@' on silicon). Interactive command
    # lines are therefore sent in small chunks with a short gap; bulk streams
    # (Flash Writer image) stay unpaced.
    PACE_CHUNK = 64
    PACE_GAP_S = 0.003

    def write(self, data: bytes, paced: bool = False) -> None:
        if not paced:
            self._write_raw(data)
            return
        for i in range(0, len(data), self.PACE_CHUNK):
            self._write_raw(data[i : i + self.PACE_CHUNK])
            time.sleep(self.PACE_GAP_S)

    def send_line(self, line: str, eol: str = "\r", paced: bool = False) -> None:
        self.write((line + eol).encode(), paced)

    def _pull(self, timeout: float) -> bool:
        data = self._read_raw(timeout)
        if not data:
            return False
        text = self._decoder.decode(data)
        if text:
            self.transcript.append(text)
            self._buf += text
        return True

    def peek(self) -> str:
        """The unconsumed console text, without consuming it."""
        return self._buf

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

    def pump(self, seconds: float) -> None:
        """Read into the buffer for `seconds` (opens the port if needed); nothing is discarded."""
        deadline = time.monotonic() + seconds
        while (remaining := deadline - time.monotonic()) > 0:
            self._pull(min(remaining, 0.2))

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


# RTL8211F(I) datasheet Rev 1.7 Table 53 notes 1-2: toggling the 3.3 V source
# needs both 3.3 V and the PHY 1.0 V at 0 V with a toggle period > 100 ms. A
# short OFF dwell or a sub-second ON blip violates it, so both are enforced here
# and not left to bench.yaml or to the step order.
MIN_OFF_S = 10.0
MIN_ON_S = 5.0
ON_SETTLE_S = 1.0  # keep reading this long after ON: the ROM banner lands within ms of the edge


class Power:
    _clock = staticmethod(time.monotonic)
    _sleep = staticmethod(time.sleep)
    _last_on: float | None = None
    on_count = 0  # ON events issued; lets a step tell whether the unit was cycled since it looked
    _defer_confirm = False  # cycle(): confirm the ON AFTER the console settle, not before

    def confirm_on(self) -> None:
        """Verify the ON took effect (subclasses that can read the output state back)."""

    @property
    def log(self) -> list[str]:
        """Every PSU command with a monotonic timestamp, for the step evidence."""
        return self.__dict__.setdefault("_log", [])

    def _note(self, what: str) -> None:
        self.log.append(f"[t={self._clock():.3f}] POWER {what}")

    def _mark_on(self) -> None:
        self._last_on = self._clock()
        self.on_count += 1

    def _wait_min_on(self) -> None:
        """Never cut power before the unit has been ON for MIN_ON_S."""
        if self._last_on is None:
            # A fresh Power may belong to a run started right after another run's
            # ON (state does not survive the process): assume it was just switched on.
            self._last_on = self._clock()
        if (rest := self._last_on + MIN_ON_S - self._clock()) > 0:
            self._note(f"waiting {rest:.3f}s (min ON {MIN_ON_S:g}s)")
            self._sleep(rest)

    def on(self) -> None:
        raise NotImplementedError

    def off(self) -> None:
        raise NotImplementedError

    def cycle(self, off_s: float = MIN_OFF_S, console: Console | None = None) -> None:
        off_s = max(off_s, MIN_OFF_S)
        self.off()
        # Keep reading the console while off: a one-shot ROM banner can land
        # right at power-on, and an unread port may deliver nothing afterwards.
        # A console error must not leave the unit off: finish the dwell, power
        # on, then report it.
        end, err = self._clock() + off_s, None
        try:
            if console is not None:
                console.pump(off_s)
        except BenchError as e:
            err = e
        if (rest := end - self._clock()) > 0:
            self._sleep(rest)
        # The ON read-back (a PSU round trip, up to ~30 s of reconnect backoff) must not run
        # between the ON edge and the first console read: the ROM banner lands within ms. So
        # on() only sends here, and the read-back follows the settle read below.
        self._defer_confirm = True
        try:
            self.on()
        finally:
            self._defer_confirm = False
        # Read continuously across the ON edge (not just up to it): whatever the
        # unit prints right at power-up is buffered for the caller's expect().
        try:
            if console is not None and err is None:
                console.pump(ON_SETTLE_S)
        except BenchError as e:
            err = e
        self.confirm_on()
        if err is not None:
            raise BenchError(f"console failed during the power cycle (power restored): {err}") from err

    def is_on(self) -> bool | None:  # None = unknowable
        return None

    def current(self) -> float | None:
        """Supply current in amps, one reading; None when this power kind cannot measure it."""
        return None

    def voltage(self) -> float | None:
        """Supply voltage in volts, one reading; None when this power kind cannot measure it."""
        return None


class ScpiPower(Power):
    """SCPI over ONE persistent raw TCP connection (the only SCPI socket in the tool).

    The SPD3303X LAN stack hangs (resets, WinError 10054) after many short
    connections and only a PSU power-cycle clears it, so the connection is
    kept open, commands are serialised under a lock with a small gap between
    them, and a failure reconnects with backoff before giving up.

    Command syntax is the bench-proven ``OUTP CH<n>,ON|OFF`` form, not the
    IEEE ``(@n)`` channel-list form; output state is read from ``SYST:STAT?``.
    """

    GAP_S = 0.05  # minimum spacing between commands
    BACKOFF_S = (3.0, 6.0, 9.0, 12.0)  # reconnect waits: 5 attempts over ~30 s

    def __init__(
        self, host: str, port: int, channel: int, connect=socket.create_connection
    ) -> None:
        self.host, self.port, self.channel = host, port, int(channel)
        if self.channel not in (1, 2):
            raise BenchError(f"SCPI power channel {channel}: only CH1 and CH2 have a SYST:STAT? "
                             "output bit, so the output state cannot be confirmed; use 1 or 2")
        self._connect = connect
        self._sock = None
        self._lock = threading.Lock()
        self._last_io: float | None = None

    def close(self) -> None:
        with self._lock:
            self._drop()

    def _drop(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _io(self, cmd: str, reply: bool) -> str:
        if self._sock is None:
            self._sock = self._connect((self.host, self.port), timeout=3)
        if self._last_io is not None and (
            rest := self.GAP_S - (self._clock() - self._last_io)
        ) > 0:
            self._sleep(rest)
        self._sock.sendall((cmd + "\n").encode())
        self._last_io = self._clock()
        if not reply:
            return ""
        data = b""
        while not data.endswith(b"\n"):
            chunk = self._sock.recv(256)
            if not chunk:
                raise ConnectionResetError("PSU closed the connection")
            data += chunk
        self._last_io = self._clock()
        return data.decode(errors="replace").strip()

    def _send(self, cmd: str, reply: bool = False) -> str:
        self._note(f"SCPI {cmd}")
        with self._lock:
            err: OSError | None = None
            for attempt in range(len(self.BACKOFF_S) + 1):
                try:
                    return self._io(cmd, reply)
                except OSError as e:
                    err = e
                    self._drop()
                    if attempt < len(self.BACKOFF_S):
                        self._note(f"SCPI reconnect in {self.BACKOFF_S[attempt]:g}s: {e}")
                        self._sleep(self.BACKOFF_S[attempt])
            raise BenchError(
                f"SCPI {self.host}:{self.port} {cmd!r}: still failing after "
                f"{len(self.BACKOFF_S) + 1} attempts: {err}"
            ) from err

    def on(self) -> None:
        try:
            self._send(f"OUTP CH{self.channel},ON")
        finally:  # the PSU may have acted even if the reply path failed
            self._mark_on()
        if not self._defer_confirm:
            self._confirm(True)

    def confirm_on(self) -> None:
        self._confirm(True)

    def off(self) -> None:
        self._wait_min_on()
        self._send(f"OUTP CH{self.channel},OFF")
        self._confirm(False)

    def _confirm(self, want: bool) -> None:
        """OUTP has no reply, so a half-dead socket can swallow it: read the
        state back and resend once before giving up."""
        for attempt in range(2):
            if self.is_on() is want:
                return
            if attempt == 0:
                self._send(f"OUTP CH{self.channel},{'ON' if want else 'OFF'}")
                if want:        # the first ON was swallowed: the ON edge is now, not an extra ON event
                    self._last_on = self._clock()
        raise BenchError(f"SCPI {self.host}:{self.port}: CH{self.channel} did not read back "
                         f"{'ON' if want else 'OFF'} after OUTP; check the PSU front panel")

    def is_on(self) -> bool | None:
        """Siglent SPD3303X has no ``OUTP?`` query (it times out); its output
        state is ``SYST:STAT?`` (hex) bit 4 for CH1, bit 5 for CH2 --
        bench-read ``0x14`` with CH1 on. None when the reply does not parse
        or the channel has no status bit."""
        if self.channel not in (1, 2):
            return None
        try:
            stat = int(self._send("SYST:STAT?", reply=True), 16)
        except (BenchError, ValueError):
            return None
        return bool(stat >> (3 + self.channel) & 1)

    def _measure(self, what: str) -> float:
        """One ``MEAS:<what>? CH<n>`` reading of the configured channel only, over the
        persistent socket. BenchError when the reply is not a finite number."""
        cmd = f"MEAS:{what}? CH{self.channel}"
        reply = self._send(cmd, reply=True)
        try:
            value = float(reply)
        except ValueError:
            value = math.nan
        if not math.isfinite(value):
            raise BenchError(f"SCPI {self.host}:{self.port} {cmd}: not a number: {reply!r}")
        return value

    def current(self) -> float:
        """One ``MEAS:CURR? CH<n>`` reading, in amps."""
        return self._measure("CURR")

    def voltage(self) -> float:
        """One ``MEAS:VOLT? CH<n>`` reading, in volts (same parsing rules as current())."""
        return self._measure("VOLT")


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
            r = self._runner(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise BenchError(f"{' '.join(argv)}: {e}") from e
        if r.returncode != 0:
            raise BenchError(
                f"{' '.join(argv)} failed ({r.returncode}): {(r.stderr or r.stdout).strip()}"
            )
        return r.stdout

    def on(self) -> None:
        self._note("labgrid on")
        try:
            self._lg("on")
        finally:
            self._mark_on()

    def off(self) -> None:
        self._wait_min_on()
        self._note("labgrid off")
        self._lg("off")

    def is_on(self) -> bool | None:
        words = re.findall(r"\b(on|off)\b", self._lg("get").lower())
        return (words[-1] == "on") if words else None


class ManualPower(Power):
    def __init__(self, operator: Operator) -> None:
        self.operator = operator

    def on(self) -> None:
        self.operator.confirm("Switch the unit's power ON.")
        self._mark_on()

    def off(self) -> None:
        self._wait_min_on()
        self.operator.confirm("Switch the unit's power OFF.")

    def cycle(self, off_s: float = MIN_OFF_S, console: Console | None = None) -> None:
        off_s = max(off_s, MIN_OFF_S)
        self._wait_min_on()
        self._note("manual cycle")
        self.operator.confirm(
            f"Power-cycle the unit: OFF, wait at least {off_s:g} s, then ON."
        )
        self._mark_on()


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
                r = self._runner(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=300)
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
        self.env: dict[str, str] = {}   # e.g. ALP_PROVISION_HOST, set once the unit's IP is known

    def _call(self, *args: str) -> str:
        interp = {".sh": ["bash"], ".py": [sys.executable]}.get(self.wrapper.suffix, [])
        argv = interp + [
            str(self.wrapper),
            *args,
        ]
        try:
            extra = {"env": {**os.environ, **self.env}} if self.env else {}
            r = self._runner(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=300, **extra)
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


DEFAULT_OFF_S = 15.0  # the V2N rails and the 5L35023B need this long to discharge
_log = logging.getLogger(__name__)
CONSOLE_SWD_TOOLS = ("swd_bb.py", "gd32_swd_flash.py")


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
    # (dir, tool filenames) the console-push SWD fallback copies to the board
    console_swd: tuple[Path, tuple[str, ...]] | None = None

    @property
    def off_s(self) -> float:
        """Power-off dwell for a cold cycle: bench.yaml power.off_s (clamped UP to
        MIN_OFF_S; a config may not shorten it), else DEFAULT_OFF_S."""
        v = float((self.raw.get("power") or {}).get("off_s", DEFAULT_OFF_S))
        if v < MIN_OFF_S:
            _log.warning("bench.yaml power.off_s=%g is below the %g s safety minimum "
                         "(RTL8211F power rules); using %g s", v, MIN_OFF_S, MIN_OFF_S)
            return MIN_OFF_S
        return v


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

    console_swd = None
    if pr and pr.get("kind") == "script":
        console_swd = (
            rel(pr["wrapper"]).parent,
            tuple(pr.get("console_tools") or CONSOLE_SWD_TOOLS),
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
        console_swd=console_swd,
    )
