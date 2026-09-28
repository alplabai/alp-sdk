# SPDX-License-Identifier: Apache-2.0
"""Hardware fakes for the scripts/provision/ tests. No bench needed.

FakeConsole script semantics: a list of ``(write_regex | None, emitted_text)``.
Entries are consumed in order. A ``None`` entry's text is emitted as soon as
it is reached (at construction, or right after the previous entry fired). An
entry with a regex fires on the next write: the written bytes (latin-1
decoded, EOL included) must ``re.search`` the regex, else AssertionError;
then its text is emitted. A write after the script is exhausted is also an
AssertionError. Note U-Boot / shells echo typed commands: put the echo in the
emitted text when the code under test relies on it.
"""

from __future__ import annotations

import hashlib
import re
from collections.abc import Callable
from pathlib import Path

from provision.bench import BenchError, Console, Operator, Power, Probe


class FakeConsole(Console):
    def __init__(
        self, script: list[tuple[str | None, str]], chunk: int | None = None
    ) -> None:
        super().__init__()
        self.script = list(script)
        self.chunk = chunk  # split emitted text into reads of this many bytes
        self.written: list[str] = []
        self._out = bytearray()
        self.closed = False
        self._emit_ready()

    def feed(self, text: str | bytes) -> None:
        """Emit text now (e.g. from FakePower's on-hook)."""
        self._out += text.encode() if isinstance(text, str) else text

    def _emit_ready(self) -> None:
        while self.script and self.script[0][0] is None:
            self.feed(self.script.pop(0)[1])

    def _read_raw(self, timeout: float) -> bytes:
        n = self.chunk or len(self._out)
        data, self._out = bytes(self._out[:n]), self._out[n:]
        return data

    def _write_raw(self, data: bytes) -> None:
        text = data.decode("latin-1")
        self.written.append(text)
        if not self.script:
            raise AssertionError(f"unexpected write after script end: {text!r}")
        rx, out = self.script[0]
        if not re.search(rx, text):
            raise AssertionError(f"write {text!r} does not match expected {rx!r}")
        self.script.pop(0)
        self.feed(out)
        self._emit_ready()

    def close(self) -> None:
        self.closed = True


class FakePower(Power):
    def __init__(
        self, on_hook: Callable[[], None] | None = None, state: bool | None = None
    ) -> None:
        self.events: list[str] = []
        self.on_hook = on_hook
        self.state = state

    def on(self) -> None:
        self.events.append("on")
        self.state = True
        if self.on_hook:
            self.on_hook()

    def off(self) -> None:
        self.events.append("off")
        self.state = False

    def cycle(self, off_s: float = 3.0) -> None:  # no sleeping in tests
        self.off()
        self.on()

    def is_on(self) -> bool | None:
        return self.state


class FakeProbe(Probe):
    """memory: {base_addr: bytes}. loadbin stores the file at addr; savebin
    returns the bytes covering [addr, addr+size), 0xFF where nothing is."""

    def __init__(
        self, dp_id_value: int = 0x0BE12477, memory: dict[int, bytes] | None = None
    ) -> None:
        self.dp_id_value = dp_id_value
        self.memory: dict[int, bytes] = dict(memory or {})
        self.calls: list[tuple] = []
        self.sessions = 0

    def dp_id(self) -> int:
        self.sessions += 1
        self.calls.append(("dp_id",))
        return self.dp_id_value

    def loadbin(self, path: Path, addr: int) -> None:
        self.sessions += 1
        self.calls.append(("loadbin", Path(path), addr))
        self.memory[addr] = Path(path).read_bytes()

    def savebin(self, path: Path, addr: int, size: int) -> None:
        self.sessions += 1
        self.calls.append(("savebin", Path(path), addr, size))
        out = bytearray(b"\xff" * size)
        for base, data in self.memory.items():
            lo, hi = max(base, addr), min(base + len(data), addr + size)
            if lo < hi:
                out[lo - addr : hi - addr] = data[lo - base : hi - base]
        Path(path).write_bytes(bytes(out))

    def reset_run(self) -> None:
        self.sessions += 1
        self.calls.append(("reset_run",))


class FakeLinux:
    """Duck-types provision.linux_target.LinuxTarget.

    responses: {regex: CmdResult | str | (rc, stdout[, stderr])}; the first
    regex that re.search-es the command answers it (dict order). A str is
    rc 0 stdout. An unmatched command is an AssertionError. files: remote
    path -> bytes, used by put/get/md5.
    """

    def __init__(self, responses: dict | None = None, files: dict[str, bytes] | None = None,
                 host: str = "fake-target", user: str = "root") -> None:  # fmt: skip
        self.responses = dict(responses or {})
        self.files: dict[str, bytes] = dict(files or {})
        self.commands: list[str] = []
        self.host, self.user = host, user

    def run(
        self,
        cmd: str,
        timeout: float = 60.0,
        check: bool = True,
        stdin_path: Path | None = None,
    ):
        from provision.linux_target import CmdResult  # lazy: owned by the linux unit

        self.commands.append(cmd)
        for rx, res in self.responses.items():
            if re.search(rx, cmd):
                if isinstance(res, str):
                    res = CmdResult(0, res, "")
                elif isinstance(res, tuple):
                    res = CmdResult(res[0], res[1], res[2] if len(res) > 2 else "")
                if check and res.rc != 0:
                    raise BenchError(f"{cmd!r} failed ({res.rc}): {res.stderr}")
                return res
        raise AssertionError(f"FakeLinux: no response scripted for {cmd!r}")

    def put(self, local: Path, remote: str) -> None:
        self.commands.append(f"put {local} {remote}")
        self.files[remote] = Path(local).read_bytes()

    def get(self, remote: str, local: Path) -> None:
        self.commands.append(f"get {remote} {local}")
        Path(local).write_bytes(self.files[remote])

    def md5(self, path: str, offset: int = 0, size: int | None = None) -> str:
        data = self.files[path][offset:]
        return hashlib.md5(data if size is None else data[:size]).hexdigest()


class FakeOperator(Operator):
    """answers are consumed in order; an exhausted list answers "" (Enter)."""

    def __init__(self, answers: list[str] | None = None) -> None:
        self.answers = list(answers or [])
        self.messages: list[str] = []
        super().__init__(ask=self._fake_ask, say=self.messages.append)

    def _fake_ask(self, prompt: str) -> str:
        self.messages.append(prompt)
        return self.answers.pop(0) if self.answers else ""
