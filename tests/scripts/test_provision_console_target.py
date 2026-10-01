"""provision.console_target: file transfer and the SWD probe over a (fake) root shell."""

from __future__ import annotations

import base64
import hashlib
import re
import shlex

import pytest

from provision import console_target as ct
from provision import steps
from provision.bench import BenchError, Console
from provision.steps import Refused
from .provision_fakes import FakeConsole
from .test_provision_steps import _bench, _ctx, _gd32_fw


class ShellConsole(Console):
    """A root shell: understands the marker framing and a tiny file model."""

    def __init__(self, corrupt_pushes: bool = False) -> None:
        super().__init__()
        self.files: dict[str, bytes] = {}
        self.lines: list[str] = []
        self.ran: list[str] = []
        self.corrupt = corrupt_pushes
        self._out = b""

    def _read_raw(self, timeout):
        data, self._out = self._out, b""
        return data

    def _write_raw(self, data: bytes) -> None:
        line = data.decode().rstrip("\r\n")
        self.lines.append(line)
        m = re.fullmatch(r'echo "@@B""(\d+)"; \( (.*)\n\) 2>&1; echo "@@E""\d+:\$\?"', line, re.S)
        assert m, line
        n, cmd = m.groups()
        out, rc = self._exec(cmd)
        # the tty echoes what was typed first; markers in the echo are split by ""
        self._out = (line + "\r\n").encode() + f"@@B{n}\r\n{out}@@E{n}:{rc}\r\n".encode()

    def _exec(self, cmd: str) -> tuple[str, int]:
        self.ran.append(cmd)
        if cmd.startswith(": > "):
            self.files[shlex.split(cmd[4:])[0]] = b""
        elif m := re.fullmatch(r"printf %s (\S+) \| base64 -d >> (.+)", cmd):
            raw = base64.b64decode(m[1])
            if self.corrupt:
                raw = raw[:-1] + b"\x00"
            self.files[shlex.split(m[2])[0]] += raw
        elif m := re.fullmatch(r"md5sum < (.+)", cmd):
            return hashlib.md5(self.files[shlex.split(m[1])[0]]).hexdigest() + "  -\n", 0
        elif m := re.fullmatch(r"base64 (.+)", cmd):
            b64 = base64.b64encode(self.files[shlex.split(m[1])[0]]).decode()
            return "\n".join(b64[i:i + 76] for i in range(0, len(b64), 76)) + "\n", 0
        elif cmd.startswith("mkdir -p"):
            pass
        elif m := re.fullmatch(r"cd (\S+) && python3 swd_bb.py", cmd):
            return "SWD id=0x0be12477\n", 0
        elif m := re.fullmatch(r"cd (\S+) && python3 gd32_swd_flash.py dump (\S+) (\S+) (\S+)", cmd):
            self.files[m[4]] = bytes(range(256)) * (int(m[3], 0) // 256)
        elif "gd32_swd_flash.py" in cmd:
            pass
        else:
            return f"unexpected: {cmd}\n", 127
        return "", 0


def test_put_chunks_under_the_tty_line_limit_and_verifies_md5(tmp_path):
    data = bytes(range(256)) * 40                     # 10 KiB -> several chunks
    src = tmp_path / "img.bin"
    src.write_bytes(data)
    sh = ShellConsole()
    ct.ConsoleTarget(sh).put(src, "/tmp/x/img.bin")
    assert sh.files["/tmp/x/img.bin"] == data
    pushes = [ln for ln in sh.lines if "base64 -d" in ln]
    assert len(pushes) == -(-len(data) // ct.RAW_CHUNK) and all(len(ln) < 4096 for ln in sh.lines)


def test_put_aborts_on_md5_mismatch(tmp_path):
    src = tmp_path / "img.bin"
    src.write_bytes(b"A" * 5000)
    with pytest.raises(BenchError, match="md5"):
        ct.ConsoleTarget(ShellConsole(corrupt_pushes=True)).put(src, "/tmp/img.bin")


def test_get_roundtrip_and_nonzero_rc(tmp_path):
    sh = ShellConsole()
    sh.files["/tmp/rb.bin"] = b"\x01\x02\xff" * 100
    t = ct.ConsoleTarget(sh)
    t.get("/tmp/rb.bin", tmp_path / "rb.bin")
    assert (tmp_path / "rb.bin").read_bytes() == sh.files["/tmp/rb.bin"]
    with pytest.raises(BenchError, match="rc=127"):
        t.run("nonsense")
    assert t.run("nonsense", check=False).rc == 127


def test_swd_probe_pushes_tools_once_and_runs_them(tmp_path):
    for f in ("swd_bb.py", "gd32_swd_flash.py"):
        (tmp_path / f).write_text("# tool\n", encoding="utf-8")
    img = tmp_path / "bl.bin"
    img.write_bytes(b"\x55" * 64)
    sh = ShellConsole()
    p = ct.ConsoleSwdProbe(ct.ConsoleTarget(sh), tmp_path, ("swd_bb.py", "gd32_swd_flash.py"))
    assert p.dp_id() == 0x0BE12477
    p.loadbin(img, 0x08000000)
    out = tmp_path / "rb.bin"
    p.savebin(out, 0x08000000, 512)
    p.reset_run()
    assert sh.files[f"{ct.REMOTE_DIR}/img.bin"] == img.read_bytes()
    assert len(out.read_bytes()) == 512
    assert any("gd32_swd_flash.py write 0x8000000" in c for c in sh.ran)
    assert sum("swd_bb.py" in ln and "base64 -d" in ln for ln in sh.lines) == 1   # pushed once


# --- Gd32Flash transport choice -----------------------------------------------------------------

def _no_net_ctx(tmp_path, swd=None, console=None):
    tmp_path.mkdir(parents=True, exist_ok=True)
    b = _bench(console=console)
    b.console_swd = swd
    return _ctx(tmp_path, bench=b, gd32_fw=_gd32_fw(tmp_path), execute=True)


def test_gd32_flash_prefers_ssh_when_the_unit_is_reachable(tmp_path):
    ctx = _no_net_ctx(tmp_path)
    ctx.linux = type("L", (), {"host": "h", "run": lambda self, *a, **k: type("R", (), {"rc": 0})()})()
    ctx.need_linux = lambda: ctx.linux
    t, probe, via = steps.Gd32Flash._transport(ctx)
    assert (t, via) == (ctx.linux, False) and probe is ctx.bench.probe


def test_gd32_flash_falls_back_to_the_console_when_there_is_no_network(tmp_path, monkeypatch):
    for f in ct_tools():
        (tmp_path / f).write_text("# tool\n", encoding="utf-8")
    monkeypatch.setattr(steps.lt, "console_login", lambda console, user: None)
    ctx = _no_net_ctx(tmp_path, swd=(tmp_path, ct_tools()))
    t, probe, via = steps.Gd32Flash._transport(ctx)
    assert via and isinstance(t, ct.ConsoleTarget) and isinstance(probe, ct.ConsoleSwdProbe)


def test_gd32_flash_refuses_without_network_or_console_tools(tmp_path):
    with pytest.raises(Refused, match="no script probe"):
        steps.Gd32Flash._transport(_no_net_ctx(tmp_path / "a"))
    with pytest.raises(Refused, match="missing"):
        steps.Gd32Flash._transport(_no_net_ctx(tmp_path / "b", swd=(tmp_path / "none", ct_tools())))


def ct_tools():
    return ("swd_bb.py", "gd32_swd_flash.py")


# --- boot_to_linux / cold boot ------------------------------------------------------------------

def test_boot_to_linux_without_ip_leaves_linux_unset_when_allowed(tmp_path, monkeypatch):
    monkeypatch.setattr(steps.lt, "console_login", lambda c, u: None)

    def no_ip(ctx, force=False):
        raise BenchError("no inet address")
    monkeypatch.setattr(steps, "connect_linux", no_ip)
    con = FakeConsole([])
    b = _bench(console=con)
    b.power.on_hook = lambda: con.feed("login: ")
    ctx = _ctx(tmp_path, bench=b, execute=True)
    steps.boot_to_linux(ctx, timeout=1, need_ip=False)
    assert ctx.linux is None
    with pytest.raises(BenchError, match="no inet"):
        steps.boot_to_linux(ctx, timeout=1)


def test_cold_boot_retries_once_when_end0_has_no_carrier(tmp_path, monkeypatch):
    boots = []

    def fake_boot(ctx, timeout=240.0, need_ip=True):
        boots.append(need_ip)
        ctx.linux = object() if len(boots) > 1 else None
        return f"boot{len(boots)} "
    monkeypatch.setattr(steps, "boot_to_linux", fake_boot)
    monkeypatch.setattr(steps.lt, "net_ifaces", lambda t: ["end0", "end1"])
    monkeypatch.setattr(steps.lt, "net_carrier", lambda t, n: len(boots) > 1)
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    ev: dict[str, str] = {}
    assert steps.ColdBootTest._cold_boot(ctx, ev) == "boot1 boot2 "
    assert ev["end0_no_carrier_retries"] == "1" and boots == [False, True]


def test_cold_boot_without_ip_but_with_carrier_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "boot_to_linux", lambda ctx, timeout=240.0, need_ip=True: "b")
    monkeypatch.setattr(steps.lt, "net_ifaces", lambda t: ["end0"])
    monkeypatch.setattr(steps.lt, "net_carrier", lambda t, n: True)
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    with pytest.raises(BenchError, match="no IP"):
        steps.ColdBootTest._cold_boot(ctx, {})
