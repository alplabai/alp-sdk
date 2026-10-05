# SPDX-License-Identifier: Apache-2.0
"""A LinuxTarget stand-in that talks over the serial console, for a unit with no network.

A unit can boot with no working gbeth port (a latched PHY, #2582), so there is
no SSH -- and the GD32 flash itself ran over SSH. This
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
import random
import re
import shlex
from pathlib import Path

from provision.bench import BenchError, Console, ExpectTimeout, Probe
from provision.linux_target import CmdResult

RUN_ATTEMPTS = 3        # a command whose begin marker never shows is re-sent (Ctrl-C first)
BEGIN_WINDOW_S = 10.0
CHUNK_WAIT_S = 10.0     # a chunk is one printf + md5sum: if its end marker takes longer it was garbled
RAW_CHUNK = 768         # bytes -> 1024 base64 chars per line, the size proven on silicon
REMOTE_DIR = "/tmp/v2n-swd"
_DECODE = "import base64,sys;open(sys.argv[2],'wb').write(base64.b64decode(open(sys.argv[1]).read()))"
_ENCODE = "import base64,sys;print(base64.encodebytes(open(sys.argv[1],'rb').read()).decode())"
# Module-wide, alphanumeric, per-attempt unique, random start per process: a stale or
# garbled echo (even one left by an earlier run) can never satisfy a later wait.
_marker = random.randrange(10**6) * 1000
_DPID = re.compile(r"id=(0x[0-9a-fA-F]{8})")


class ConsoleTarget:
    host = "console"

    def __init__(self, console: Console) -> None:
        self.console = console
        self.last_began = True      # did the last run()'s begin marker show (the command ran)?
        self.last_out = ""          # that run()'s output

    def run(self, cmd: str, timeout: float = 60.0, check: bool = True,
            stdin_path: Path | None = None, long_running: bool = False) -> CmdResult:
        """``long_running`` (a flash/dd write): never Ctrl-C and never re-send. A re-send could
        write twice and the Ctrl-C would kill a write that is in progress, so a begin marker
        that does not show is an error for the operator, not a retry."""
        if stdin_path is not None:
            raise BenchError("console target cannot stream a file on stdin; use put()")
        global _marker
        for attempt in range(1 if long_running else RUN_ATTEMPTS):
            _marker += 1                       # a fresh marker per attempt: a stale one may still be buffered
            n = _marker
            self.console.drain(0.1, 2.0)
            # `&&`, not `;`: a garbled echo word must not let the command run unannounced
            # (a retry would then run it twice, e.g. a flash write).
            self.console.send_line(f'echo "ALPB""{n}" && ( {cmd}\n) 2>&1; echo "ALPE""{n}:$?"', paced=True)
            try:
                # a corrupted command line never prints its begin marker
                self.console.expect(rf"ALPB{n}\r?\n", BEGIN_WINDOW_S)
                self.last_began = True
                break
            except ExpectTimeout as e:
                if re.search(rf"ALPE{n}:\d+", self.console.peek()):
                    # No begin marker but an end marker: a garbled echo word, the `&&` guard kept the
                    # command from running (rc=127). Never re-run it from here.
                    self.last_began = False
                    break
                if long_running or attempt == RUN_ATTEMPTS - 1:
                    tries = "1 try (long-running: never re-sent)" if long_running else f"{RUN_ATTEMPTS} tries"
                    raise BenchError(f"console command never started after {tries} "
                                     f"(corrupted RX?): {cmd[:80]!r}: {e.tail[-120:]!r}") from e
                self.console.write(b"\x03")
                self.console.drain(0.5, 5.0)
        m = self.console.expect(rf"(?s)(?P<out>.*?)ALPE{n}:(?P<rc>\d+)", timeout)
        out, rc = m.group("out").replace("\r\n", "\n"), int(m.group("rc"))
        self.last_out = out
        if check and rc != 0:
            raise BenchError(f"rc={rc}: {cmd[:200]}: {out.strip()[-500:]}")
        return CmdResult(rc, out, "")

    def put(self, local: Path, remote: str) -> None:
        data, q = Path(local).read_bytes(), shlex.quote(remote)
        # The board image has no base64 binary: collect the text, decode with python3.
        # Each chunk goes to its own numbered file and is md5-checked on arrival, so a
        # byte corrupted on the way is re-sent alone; the parts are assembled at the end.
        self._run_unannounced(f"rm -f {q}.p* {q}.b64")
        for idx, i in enumerate(range(0, len(data), RAW_CHUNK)):
            b64 = base64.b64encode(data[i:i + RAW_CHUNK]).decode("ascii")
            part, want = f"{q}.p{idx:05d}", hashlib.md5(b64.encode()).hexdigest()
            for attempt in range(RUN_ATTEMPTS):
                try:
                    got = self.run(f"printf %s {b64} > {part}; md5sum < {part}",
                                   timeout=CHUNK_WAIT_S, check=False).stdout.split()
                except ExpectTimeout:
                    got = self._resync_verify(part)
                if got and got[0] == want:
                    break
            else:
                raise BenchError(f"console push {local}: chunk {idx} still corrupted after {RUN_ATTEMPTS} tries")
        self._run_unannounced(f"cat {q}.p* > {q}.b64 && rm -f {q}.p* && "
                 f"python3 -c {shlex.quote(_DECODE)} {q}.b64 {q} && rm {q}.b64")
        self._check_md5(q, data, f"push {local}")

    def _run_unannounced(self, cmd: str) -> CmdResult:
        """run() for put()'s housekeeping: re-sent on rc=127 only when the command did not run.
        That is (a) no begin marker (a garbled echo word, the `&&` guard skipped the command) or
        (b) the shell says `<word>: not found` for a word that is not in the command we sent (RX
        garbled the command word itself). A tool of ours that is missing on the board is named in
        the command: that is the command running and failing, and is never re-run."""
        for attempt in range(RUN_ATTEMPTS):
            try:
                return self.run(cmd)
            except BenchError as e:
                if (not str(e).startswith("rc=127") or not self._never_ran(cmd)
                        or attempt == RUN_ATTEMPTS - 1):
                    raise
        raise AssertionError("unreachable")

    def _never_ran(self, cmd: str) -> bool:
        if not self.last_began:
            return True
        m = re.search(r"(\S+): (?:command )?not found", self.last_out)
        return m is not None and m.group(1) not in cmd

    def _resync_verify(self, part: str) -> list[str]:
        """The end marker of a chunk never showed (the command or the marker was garbled on
        the way in): clear the shell line, then re-read the part file's md5 under a fresh
        marker, which doubles as the resync probe. [] if even that gets no answer."""
        self.console.write(b"\x03")
        self.console.drain(0.5, 5.0)
        try:
            return self.run(f"md5sum < {part}", timeout=CHUNK_WAIT_S, check=False).stdout.split()
        except ExpectTimeout:
            return []

    def get(self, remote: str, local: Path) -> None:
        q = shlex.quote(remote)
        out = self.run(f"python3 -c {shlex.quote(_ENCODE)} {q}", timeout=600.0).stdout
        try:
            data = base64.b64decode(re.sub(r"\s+", "", out), validate=True)
        except ValueError as e:
            raise BenchError(f"console pull {remote}: undecodable base64: {e}") from e
        self._check_md5(q, data, f"pull {remote}")
        Path(local).write_bytes(data)

    def md5(self, path: str) -> str:
        """md5 of the file on the board ('' when md5sum yields nothing)."""
        r = self.run(f"md5sum < {shlex.quote(path)}", check=False)
        got = r.stdout.split()
        return got[0] if r.rc == 0 and got else ""

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

    def _py(self, args: str, timeout: float = 600.0, long_running: bool = False) -> str:
        if not self._pushed:
            self.t.run(f"mkdir -p {REMOTE_DIR}")
            for f in self.tools:
                self.t.put(self.tools_dir / f, f"{REMOTE_DIR}/{f}")
            self._pushed = True
        extra = {"long_running": True} if long_running else {}    # a flash/dd write: never re-sent
        return self.t.run(f"cd {REMOTE_DIR} && python3 {args}", timeout=timeout, **extra).stdout

    def dp_id(self) -> int:
        out = self._py("swd_bb.py")
        m = _DPID.search(out)
        if not m:
            raise BenchError(f"console SWD probe: no IDCODE in {out!r}")
        return int(m.group(1), 16)

    def loadbin(self, path: Path, addr: int) -> None:
        self.t.put(Path(path), f"{REMOTE_DIR}/img.bin")
        self._py(f"gd32_swd_flash.py write {addr:#x} {REMOTE_DIR}/img.bin", long_running=True)

    def savebin(self, path: Path, addr: int, size: int) -> None:
        self._py(f"gd32_swd_flash.py dump {addr:#x} {size:#x} {REMOTE_DIR}/rb.bin")
        self.t.get(f"{REMOTE_DIR}/rb.bin", Path(path))
        if Path(path).stat().st_size != size:
            raise BenchError(f"console SWD savebin: {size} bytes wanted, got {Path(path).stat().st_size}")

    def reset_run(self) -> None:
        self._py("gd32_swd_flash.py reset")
