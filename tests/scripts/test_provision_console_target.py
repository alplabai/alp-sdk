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

    def __init__(self, corrupt_pushes: bool = False, flip_in_first: int = 0, seed: int = 1) -> None:
        super().__init__()
        # flip_in_first=N: one random bit of the first N bytes received is flipped (once)
        import random
        self._flip_at = random.Random(seed).randrange(flip_in_first) if flip_in_first else None
        self._rx_count = 0
        self.flipped = 0
        self.files: dict[str, bytes] = {}
        self.lines: list[str] = []
        self.ran: list[str] = []
        self.corrupt = corrupt_pushes
        self.flashed: dict[int, bytes] = {}
        self._out = b""

    def _read_raw(self, timeout):
        data, self._out = self._out, b""
        return data

    def _write_raw(self, data: bytes) -> None:
        if data == b"\x03":
            self._acc = b""
            self._out += b"^C\r\nroot@unit:~# "
            return
        if self._flip_at is not None and self._rx_count <= self._flip_at < self._rx_count + len(data):
            b = bytearray(data)
            b[self._flip_at - self._rx_count] ^= 1 << (self._flip_at % 8)
            data, self.flipped = bytes(b), self.flipped + 1
        self._rx_count += len(data)
        self._acc = getattr(self, "_acc", b"") + data
        if not self._acc.endswith(b"\r"):       # paced writes arrive in chunks
            return
        line, self._acc = self._acc.decode("latin-1").rstrip("\r\n"), b""
        self.lines.append(line)
        m = re.fullmatch(r'(\S+) "ALPB""(\d+)" && \( (.*)\n\) 2>&1; echo "ALPE""\d+:\$\?"', line, re.S)
        if m and m.group(1) != "echo":             # garbled echo word: && skips the command, $? = 127
            self._out = (line + f"\r\n-sh: {m.group(1)}: not found\r\nALPE{m.group(2)}:127\r\n").encode("latin-1")
            return
        if not m:                                  # a corrupted line: the shell just complains
            self._out += (line + "\r\n-sh: syntax error\r\nroot@unit:~# ").encode("latin-1")
            return
        _, n, cmd = m.groups()
        out, rc = self._exec(cmd)
        begin = "" if getattr(self, "lose_begin", False) else f"ALPB{n}\r\n"   # RX corrupted the marker
        # the tty echoes what was typed first; markers in the echo are split by ""
        self._out = (line + "\r\n").encode("latin-1") + f"{begin}{out}ALPE{n}:{rc}\r\n".encode()

    def _exec(self, cmd: str) -> tuple[str, int]:
        self.ran.append(cmd)
        if cmd.startswith("rm -f "):
            pass
        elif m := re.fullmatch(r"printf %s (\S+) > (\S+); md5sum < (\S+)", cmd):
            self.files[shlex.split(m[2])[0]] = m[1].encode()
            return hashlib.md5(m[1].encode()).hexdigest() + "  -\n", 0
        elif m := re.match(r"cat (\S+)\.p\* > (\S+) && rm -f \S+ && (python3 -c .*)", cmd):
            base = shlex.split(m[1])[0] + ".p"
            parts = sorted(k for k in self.files if k.startswith(base))
            self.files[shlex.split(m[2])[0]] = b"".join(self.files.pop(k) for k in parts)
            src, dst = shlex.split(m[3].split(" && ")[0])[3:5]
            raw = base64.b64decode(self.files.pop(src))
            self.files[dst] = raw[:-1] + b"\x00" if self.corrupt else raw
        elif re.search(r"(^|[ |;&])base64 ", cmd):
            return "-sh: base64: command not found\n", 127      # the board image has none
        elif m := re.fullmatch(r"md5sum < (.+)", cmd):
            return hashlib.md5(self.files[shlex.split(m[1])[0]]).hexdigest() + "  -\n", 0
        elif cmd.startswith("python3 -c ") and "encodebytes" in cmd:
            return base64.encodebytes(self.files[shlex.split(cmd)[3]]).decode(), 0
        elif cmd.startswith("mkdir -p"):
            pass
        elif m := re.fullmatch(r"cd (\S+) && python3 swd_bb.py", cmd):
            return "SWD id=0x0be12477\n", 0
        elif m := re.fullmatch(r"cd (\S+) && python3 gd32_swd_flash.py dump (\S+) (\S+) (\S+)", cmd):
            img = self.flashed.get(int(m[2], 0), bytes(range(256)) * (int(m[3], 0) // 256))
            self.files[m[4]] = img[:int(m[3], 0)]
        elif m := re.fullmatch(r"cd (\S+) && python3 gd32_swd_flash.py write (\S+) (\S+)", cmd):
            self.flashed[int(m[2], 0)] = self.files[m[3]]
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
    pushes = [ln for ln in sh.lines if "printf %s" in ln]
    assert len(pushes) == -(-len(data) // ct.RAW_CHUNK) and all(len(ln) < 1200 for ln in pushes)
    assert not any(re.search(r"(^|[ |;&])base64 ", c) for c in sh.ran)    # the board has no base64 binary


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
    p.savebin(out, 0x08000000, 64)
    p.reset_run()
    assert sh.files[f"{ct.REMOTE_DIR}/img.bin"] == img.read_bytes()
    assert out.read_bytes() == img.read_bytes()
    assert any("gd32_swd_flash.py write 0x8000000" in c for c in sh.ran)
    assert sum("swd_bb.py" in ln and ".b64" in ln for ln in sh.lines) >= 1


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

    def no_ip(ctx, force=False, **kw):
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


def _phy_boot_patches(monkeypatch, carrier_after_retry):
    boots = []

    def fake_boot(ctx, timeout=240.0, need_ip=True):
        boots.append(need_ip)
        ctx.linux = object() if len(boots) > 1 else None
        return f"boot{len(boots)} "
    monkeypatch.setattr(steps, "boot_to_linux", fake_boot)
    monkeypatch.setattr(steps.lt, "net_ifaces", lambda t: ["end0", "end1"])
    monkeypatch.setattr(steps.lt, "net_carrier", lambda t, n: len(boots) > 1 and carrier_after_retry)
    return boots


def test_cold_boot_retries_once_when_end0_has_no_carrier(tmp_path, monkeypatch):
    boots = _phy_boot_patches(monkeypatch, carrier_after_retry=True)
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    ev: dict[str, str] = {}
    assert steps.ColdBootTest._cold_boot(ctx, ev) == "boot1 boot2 "
    assert ev["end0_no_carrier_retries"] == "1" and boots == [False, False]


def test_cold_boot_fails_when_end0_is_still_down_after_the_retry(tmp_path, monkeypatch):
    _phy_boot_patches(monkeypatch, carrier_after_retry=False)    # an IP is present (end1) but proves nothing
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    with pytest.raises(BenchError, match="still has no carrier"):
        steps.ColdBootTest._cold_boot(ctx, {})


def test_cold_boot_without_ip_but_with_carrier_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "boot_to_linux", lambda ctx, timeout=240.0, need_ip=True: "b")
    monkeypatch.setattr(steps.lt, "net_ifaces", lambda t: ["end0"])
    monkeypatch.setattr(steps.lt, "net_carrier", lambda t, n: True)
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    with pytest.raises(BenchError, match="no IP"):
        steps.ColdBootTest._cold_boot(ctx, {})


# --- Power.cycle never leaves the unit off ------------------------------------------------------

def test_cycle_powers_back_on_when_the_console_dies(monkeypatch):
    from provision import bench

    events = []

    class P(bench.Power):
        def on(self): events.append("on")
        def off(self): events.append("off")

    class Dead(Console):
        def _read_raw(self, timeout):
            raise BenchError("port vanished")
    with pytest.raises(BenchError, match="power restored"):
        p = P()
        p._sleep, p._clock = (lambda s: None), (lambda: 0.0)
        p.cycle(0.01, Dead())
    assert events == ["off", "on"]


def test_get_rejects_garbage_base64(tmp_path):
    class Junk(ShellConsole):
        def _exec(self, cmd):
            return ("not base64 !!\n", 0) if "encodebytes" in cmd else super()._exec(cmd)
    with pytest.raises(BenchError, match="undecodable"):
        ct.ConsoleTarget(Junk()).get("/tmp/x", tmp_path / "x")


# --- boot_sd_linux with no IP ---------------------------------------------------------------------

def test_boot_sd_linux_without_ip_does_not_fail(tmp_path, monkeypatch):
    sh = ShellConsole()
    ctx = _ctx(tmp_path, bench=_bench(console=sh), execute=True)

    def boot(ctx_, timeout=240.0, need_ip=True):
        assert need_ip is False
        ctx_.linux = None
        return "DRAM:  3.9 GiB\n"
    monkeypatch.setattr(steps, "boot_to_linux", boot)
    monkeypatch.setattr(steps.lt, "root_device", lambda t: "mmcblk0p2")
    monkeypatch.setattr(steps.lt, "resolve_emmc", lambda t: "/dev/mmcblk1")
    monkeypatch.setattr(steps, "som_presence_problems", lambda c, t=None: [])
    r = steps.BootSdLinux().run(ctx)
    assert "no network yet" in r.detail and r.evidence["network"].startswith("none yet")


# --- gd32_flash over the console ----------------------------------------------------------------------

def _console_flash_ctx(tmp_path, monkeypatch, console):
    for f in ct_tools():
        (tmp_path / f).write_text("# tool\n", encoding="utf-8")
    b = _bench(console=console)
    b.console_swd = (tmp_path, ct_tools())
    monkeypatch.setattr(steps.lt, "console_login", lambda c, u: None)
    monkeypatch.setattr(steps.lt, "act88760_gpio4_held", lambda t, bus: False)
    monkeypatch.setattr(steps.lt, "gd32_bridge_version", lambda t, bus, addr: (0, 14, 0))
    return _ctx(tmp_path / "ctx", bench=b, gd32_fw=_gd32_fw(tmp_path), execute=True)


def test_gd32_flash_end_to_end_over_the_console(tmp_path, monkeypatch):
    sh = ShellConsole()
    ctx = _console_flash_ctx(tmp_path, monkeypatch, sh)

    def reboot(ctx_, ev):
        ctx_.linux = type("L", (), {"host": "192.0.2.9"})()
        return "boot"
    monkeypatch.setattr(steps, "cold_boot_phy_retry", reboot)
    r = steps.Gd32Flash().run(ctx)
    assert r.status == "done", r.detail
    assert r.evidence["gd32_flash_transport"].startswith("console")
    assert r.evidence["gd32_dp_id"] == "0x0be12477" and r.evidence["network_after_gd32"] == "192.0.2.9"
    assert sorted(sh.flashed) == [a for _n, a, _k in steps.GD32_IMAGES]
    assert not any(re.search(r"(^|[ |;&])base64 ", c) for c in sh.ran)


def test_gd32_flash_keeps_its_evidence_when_the_recheck_has_no_network(tmp_path, monkeypatch):
    ctx = _console_flash_ctx(tmp_path, monkeypatch, ShellConsole())

    def reboot(ctx_, ev):
        raise BenchError("end0 still has no carrier")
    monkeypatch.setattr(steps, "cold_boot_phy_retry", reboot)
    r = steps.Gd32Flash().run(ctx)
    assert r.status == "failed" and r.evidence["gd32_dp_id"] == "0x0be12477"
    assert r.evidence["network_after_gd32"].startswith("none:")


def test_console_path_checks_the_emmc_cid_before_touching_anything(tmp_path, monkeypatch):
    sh = ShellConsole()
    ctx = _console_flash_ctx(tmp_path, monkeypatch, sh)
    ctx.state["cid_anchor"] = "aa" + "00" * 15
    monkeypatch.setattr(steps.lt, "read_emmc_cid", lambda t: "bb" + "00" * 15)
    with pytest.raises(Refused, match="another unit"):
        steps.Gd32Flash().run(ctx)
    assert sh.flashed == {} and not any("swd_bb" in c for c in sh.ran)


# ---- login CPR query, paced writes, echo verification -----------------------------

LONG = "echo " + " ".join(["word"] * 20)      # > 64 bytes, full of spaces


class _Tty(Console):
    """A shell console that, after login, asks for the cursor position (the image's
    profile runs a resize) and garbles an unpaced burst: space -> '@' in any single
    write longer than 64 bytes, once. Chunked (paced) writes arrive intact."""

    def __init__(self, garble_every_burst=False):
        super().__init__()
        self._out = b"unit login: "
        self.writes, self.cpr_replies, self.garbled = [], 0, 0
        self.garble_every_burst = garble_every_burst
        self._line = b""
        self.cmds = []
        self.polls, self.starting_polls = 0, 0

    def _read_raw(self, timeout):
        data, self._out = self._out, b""
        return data

    def _write_raw(self, data):
        self.writes.append(data)
        if data == b"\x03":
            self._line = b""
            self._out += b"^C\r\nroot@unit:~# "
            return
        if data.endswith(b"R") and data.startswith(b"\x1b["):
            self.cpr_replies += 1
            self._out += b"root@unit:~# "
            return
        if len(data) > 64 and (self.garble_every_burst or not self.garbled):
            self.garbled += 1
            data = data.replace(b" ", b"@")
        self._line += data
        if self._line.endswith(b"\r"):
            line, self._line = self._line.rstrip(b"\r"), b""
            if not line:
                return                      # a bare Enter at the login prompt: nothing new
            self._out += line + b"\r\n"
            if line == b"":
                pass
            elif line == b"root":
                self._out += b"\x1b7\x1b[r\x1b[999;999H\x1b[6n"   # resize: waits for our reply
            elif line.startswith(b"export TERM"):
                self._out += b"root@unit:~# "
            elif line == b"systemctl is-system-running":
                self.polls += 1
                self._out += (b"running" if self.polls > self.starting_polls else b"starting") + b"\r\nroot@unit:~# "
            else:
                self.cmds.append(line.decode())
                self._out += b"out\r\nroot@unit:~# "


def test_console_login_answers_the_cursor_position_query_then_sets_term_dumb(monkeypatch):
    from provision import linux_target as lt
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    tty = _Tty()
    lt.console_login(tty)
    assert tty.cpr_replies == 1
    assert any(w.startswith(b"export TERM=dumb") for w in tty.writes)
    assert tty.writes.index(b"\x1b[24;80R") < next(i for i, w in enumerate(tty.writes) if w.startswith(b"export"))


def test_command_lines_are_sent_in_small_paced_chunks_and_survive_the_garbling(monkeypatch):
    from provision import linux_target as lt
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    monkeypatch.setattr(Console, "PACE_GAP_S", 0.0)
    tty = _Tty()
    lt.console_login(tty)
    lt.send_checked(tty, LONG)
    assert all(len(w) <= Console.PACE_CHUNK for w in tty.writes)
    assert tty.garbled == 0                         # paced chunks stay under the garble threshold


def test_echo_mismatch_sends_ctrl_c_and_resends_once(monkeypatch):
    from provision import linux_target as lt
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    monkeypatch.setattr(Console, "PACE_GAP_S", 0.0)
    monkeypatch.setattr(Console, "PACE_CHUNK", 4096)     # unpaced on purpose: first burst gets garbled
    tty = _Tty()
    lt.console_login(tty)
    lt.send_checked(tty, LONG, echo_timeout=0.05)
    assert tty.garbled == 1 and b"\x03" in tty.writes
    assert tty.cmds[-1] == LONG and tty.cmds.count(LONG) == 1   # resent intact exactly once


def test_echo_mismatch_twice_is_an_error(monkeypatch):
    from provision import linux_target as lt
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    monkeypatch.setattr(Console, "PACE_GAP_S", 0.0)
    monkeypatch.setattr(Console, "PACE_CHUNK", 4096)
    tty = _Tty(garble_every_burst=True)
    lt.console_login(tty)
    with pytest.raises(BenchError, match="never matched"):
        lt.send_checked(tty, LONG, echo_timeout=0.05)


# ---- settle wait + robust run/put against a flaky early console ---------------------

def _fast(monkeypatch):
    from provision import linux_target as lt
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    monkeypatch.setattr(lt, "SYSTEM_POLL_S", 0.0)
    monkeypatch.setattr(Console, "PACE_GAP_S", 0.0)
    monkeypatch.setattr(ct, "BEGIN_WINDOW_S", 0.1)
    return lt


def test_login_waits_until_systemd_stops_starting(monkeypatch):
    lt = _fast(monkeypatch)
    tty = _Tty()
    tty.starting_polls = 3
    lt.console_login(tty)
    assert tty.polls == 4                           # three "starting" answers, then "running"


def test_login_proceeds_after_the_settle_cap(monkeypatch):
    lt = _fast(monkeypatch)
    monkeypatch.setattr(lt, "SYSTEM_SETTLE_CAP_S", 0.05)
    tty = _Tty()
    tty.starting_polls = 10 ** 6
    lt.console_login(tty)                           # never settles: gives up waiting, no error
    assert tty.polls >= 1


@pytest.mark.parametrize("seed", range(12))
def test_run_survives_one_flipped_bit_in_the_first_bytes(seed, monkeypatch):
    _fast(monkeypatch)
    sh = ShellConsole(flip_in_first=50, seed=seed)
    assert ct.ConsoleTarget(sh).run("ls /tmp", check=False).rc == 127     # ran, got a real rc
    assert sh.flipped == 1


@pytest.mark.parametrize("seed", range(12))
def test_put_survives_one_flipped_bit_and_resends_only_the_bad_chunk(seed, tmp_path, monkeypatch):
    _fast(monkeypatch)
    data = bytes(range(256)) * 12
    src = tmp_path / "img.bin"
    src.write_bytes(data)
    sh = ShellConsole(flip_in_first=400, seed=seed)
    ct.ConsoleTarget(sh).put(src, "/tmp/x/img.bin")
    assert sh.files["/tmp/x/img.bin"] == data
    assert sh.flipped == 1


def test_garbled_echo_word_never_runs_the_command_and_is_not_retried(monkeypatch):
    sh = ShellConsole()
    orig, flipped = sh._write_raw, []

    def garble(data):
        if data.startswith(b"echo") and not flipped:
            flipped.append(1)
            data = b"ecXo" + data[4:]
        orig(data)
    monkeypatch.setattr(sh, "_write_raw", garble)
    monkeypatch.setattr(ct, "BEGIN_WINDOW_S", 0.05)
    with pytest.raises(BenchError, match="rc=127"):
        ct.ConsoleTarget(sh).run("cd /x && python3 gd32_swd_flash.py write 0x0 /tmp/f")
    assert sh.ran == [] and len([ln for ln in sh.lines if "ALPB" in ln]) == 1   # no resend


def test_lost_begin_marker_with_the_end_marker_present_does_not_rerun(monkeypatch):
    sh = ShellConsole()
    sh.lose_begin = True
    monkeypatch.setattr(ct, "BEGIN_WINDOW_S", 0.05)
    r = ct.ConsoleTarget(sh).run("mkdir -p /x", check=False)
    assert r.rc == 0 and len(sh.ran) == 1
