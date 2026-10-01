# SPDX-License-Identifier: Apache-2.0
"""A LinuxTarget stand-in that talks over the serial console, for a unit with no network.

A blank GD32 leaves both gbeth ports dead (no RX clock), so there is no SSH
until the GD32 is flashed -- and the GD32 flash itself ran over SSH. This
module closes that loop: ``ConsoleTarget`` has the ``run``/``put``/``get``
surface the provisioning steps use, over a logged-in root shell on the
console, and ``ConsoleSwdProbe`` is the bench's SWD probe wrapper re-done on
top of it (push the bit-bang tools, run them on the board, pull results).

Console framing: every command is wrapped between two markers whose echoed
copy (the tty echoes what we type) cannot match the marker regex, because
the typed form splits the marker with ``""``. Files travel as base64 in
1024-character chunks (well under the tty's 4096-byte line limit), one marker wait per chunk,
and are decoded with python3 (the board image has no ``base64``) and checked with ``md5sum`` on the board against the host's digest.
"""

from __future__ import annotations

import base64
import hashlib
import re
import shlex
from pathlib import Path

from provision.bench import BenchError, Console, Probe
from provision.linux_target import CmdResult

RAW_CHUNK = 768         # bytes -> 1024 base64 chars per line, the size proven on silicon
REMOTE_DIR = "/tmp/v2n-swd"
_DECODE = "import base64,sys;open(sys.argv[2],'wb').write(base64.b64decode(open(sys.argv[1]).read()))"
_ENCODE = "import base64,sys;print(base64.encodebytes(open(sys.argv[1],'rb').read()).decode())"
_marker = 0   # module-wide: a second ConsoleTarget must never reuse a marker still in the buffer
_DPID = re.compile(r"id=(0x[0-9a-fA-F]{8})")


class ConsoleTarget:
    host = "console"

    def __init__(self, console: Console) -> None:
        self.console = console

    def run(self, cmd: str, timeout: float = 60.0, check: bool = True,
            stdin_path: Path | None = None) -> CmdResult:
        if stdin_path is not None:
            raise BenchError("console target cannot stream a file on stdin; use put()")
        global _marker
        _marker += 1
        n = _marker
        self.console.drain(0.1, 2.0)
        self.console.send_line(
            f'echo "@@B""{n}"; ( {cmd}\n) 2>&1; echo "@@E""{n}:$?"')
        m = self.console.expect(
            rf"(?s)@@B{n}\r?\n(?P<out>.*?)@@E{n}:(?P<rc>\d+)", timeout)
        out, rc = m.group("out").replace("\r\n", "\n"), int(m.group("rc"))
        if check and rc != 0:
            raise BenchError(f"rc={rc}: {cmd[:200]}: {out.strip()[-500:]}")
        return CmdResult(rc, out, "")

    def put(self, local: Path, remote: str) -> None:
        data, q = Path(local).read_bytes(), shlex.quote(remote)
        # The board image has no base64 binary: collect the text, decode with python3.
        self.run(f": > {q}.b64")
        for i in range(0, len(data), RAW_CHUNK):
            b64 = base64.b64encode(data[i:i + RAW_CHUNK]).decode("ascii")
            self.run(f"printf %s {b64} >> {q}.b64")
        self.run(f"python3 -c {shlex.quote(_DECODE)} {q}.b64 {q} && rm {q}.b64")
        self._check_md5(q, data, f"push {local}")

    def get(self, remote: str, local: Path) -> None:
        q = shlex.quote(remote)
        out = self.run(f"python3 -c {shlex.quote(_ENCODE)} {q}", timeout=600.0).stdout
        try:
            data = base64.b64decode(re.sub(r"\s+", "", out), validate=True)
        except ValueError as e:
            raise BenchError(f"console pull {remote}: undecodable base64: {e}") from e
        self._check_md5(q, data, f"pull {remote}")
        Path(local).write_bytes(data)

    def _check_md5(self, q: str, data: bytes, what: str) -> None:
        got = self.run(f"md5sum < {q}").stdout.split()
        want = hashlib.md5(data).hexdigest()
        if not got or got[0] != want:
            raise BenchError(f"console {what}: md5 on the board {got[:1]} != host {want}")


class ConsoleSwdProbe(Probe):
    """bench/v2n-swd's probe wrapper, run through ConsoleTarget instead of ssh/scp."""

    def __init__(self, target: ConsoleTarget, tools_dir: Path, tools: tuple[str, ...]) -> None:
        self.t, self.tools_dir, self.tools = target, Path(tools_dir), tools
        self._pushed = False

    def _py(self, args: str, timeout: float = 600.0) -> str:
        if not self._pushed:
            self.t.run(f"mkdir -p {REMOTE_DIR}")
            for f in self.tools:
                self.t.put(self.tools_dir / f, f"{REMOTE_DIR}/{f}")
            self._pushed = True
        return self.t.run(f"cd {REMOTE_DIR} && python3 {args}", timeout=timeout).stdout

    def dp_id(self) -> int:
        out = self._py("swd_bb.py")
        m = _DPID.search(out)
        if not m:
            raise BenchError(f"console SWD probe: no IDCODE in {out!r}")
        return int(m.group(1), 16)

    def loadbin(self, path: Path, addr: int) -> None:
        self.t.put(Path(path), f"{REMOTE_DIR}/img.bin")
        self._py(f"gd32_swd_flash.py write {addr:#x} {REMOTE_DIR}/img.bin")

    def savebin(self, path: Path, addr: int, size: int) -> None:
        self._py(f"gd32_swd_flash.py dump {addr:#x} {size:#x} {REMOTE_DIR}/rb.bin")
        self.t.get(f"{REMOTE_DIR}/rb.bin", Path(path))
        if Path(path).stat().st_size != size:
            raise BenchError(f"console SWD savebin: {size} bytes wanted, got {Path(path).stat().st_size}")

    def reset_run(self) -> None:
        self._py("gd32_swd_flash.py reset")
