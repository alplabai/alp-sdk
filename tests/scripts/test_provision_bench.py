# SPDX-License-Identifier: Apache-2.0
"""scripts/provision/bench.py + the shared fakes. No hardware."""

from __future__ import annotations

import re
import socket
import subprocess
import threading
from pathlib import Path

import pytest
from provision import bench
from provision.bench import BenchError, ExpectTimeout

from .provision_fakes import FakeConsole, FakeLinux, FakeOperator, FakePower, FakeProbe

# ---------------------------------------------------------------- Console


def test_expect_consumes_through_match_and_keeps_rest():
    c = FakeConsole(
        [(None, "boot noise\r\nHit any key to stop autoboot:  2 \r\n=> tail")]
    )
    m = c.expect(r"Hit any key", 1.0)
    assert m.string[: m.start()] == "boot noise\r\n"
    m2 = c.expect(r"=> ", 1.0)
    assert m2.string[: m2.start()] == " to stop autoboot:  2 \r\n"
    assert c.drain(quiet_s=0.01) == "tail"
    assert "".join(c.transcript).endswith("=> tail")


def test_expect_decodes_utf8_split_across_reads():
    c = FakeConsole([(None, "temp 25°C ok")], chunk=1)  # the degree sign is 2 bytes
    c.expect("ok", 1.0)
    assert "".join(c.transcript) == "temp 25°C ok"


def test_expect_timeout_carries_pattern_and_tail():
    c = FakeConsole([(None, "x" * 5000 + "END")])
    with pytest.raises(ExpectTimeout) as e:
        c.expect(r"never", 0.05)
    assert e.value.pattern == "never"
    assert len(e.value.tail) == 4096 and e.value.tail.endswith("END")
    assert isinstance(e.value, BenchError)


def test_expect_any_returns_earliest_match():
    c = FakeConsole([(None, "login: ... => ")])
    key, _ = c.expect_any({"prompt": r"=> ", "login": r"login:"}, 1.0)
    assert key == "login"
    key, _ = c.expect_any({"prompt": r"=> ", "login": r"login:"}, 1.0)
    assert key == "prompt"


def test_fake_console_scripted_writes():
    c = FakeConsole(
        [(None, "=> "), (r"^version\r$", "version\r\nU-Boot 2024.07\r\n=> ")]
    )
    c.expect("=> ", 1.0)
    c.send_line("version")
    c.expect("=> ", 1.0)
    assert c.written == ["version\r"]
    with pytest.raises(AssertionError, match="after script end"):
        c.send_line("reset")


def test_fake_console_rejects_unexpected_write():
    c = FakeConsole([(r"^md\.l", "")])
    with pytest.raises(AssertionError, match="does not match"):
        c.send_line("reset")


def _serve_once(handler):
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    port = srv.getsockname()[1]

    def loop():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            with conn:
                handler(conn)

    threading.Thread(target=loop, daemon=True).start()
    return srv, port


def test_tcp_console_roundtrip_and_close():
    def handler(conn):
        conn.sendall(b"=> ")
        data = conn.recv(100)
        conn.sendall(data.upper() + b"=> ")

    srv, port = _serve_once(handler)
    try:
        c = bench.TcpConsole("127.0.0.1", port)
        c.expect("=> ", 2.0)
        c.send_line("ping")
        m = c.expect("=> ", 2.0)
        assert m.string[: m.start()] == "PING\r"
        with pytest.raises(BenchError, match="closed"):
            c.expect("never", 2.0)
        c.close()
    finally:
        srv.close()


# ---------------------------------------------------------------- Operator / Power


def test_operator_confirm_and_abort():
    op = FakeOperator(["", "abort", "  E1M-X  "])
    op.confirm("Set DSW1")
    with pytest.raises(BenchError, match="aborted"):
        op.confirm("Insert SD")
    assert op.ask("Serial?") == "E1M-X"
    assert "Set DSW1" in op.messages


def test_manual_power_asks_operator():
    op = FakeOperator()
    p = bench.ManualPower(op)
    p.cycle(5)
    assert p.is_on() is None
    assert any("wait at least 5 s" in m for m in op.messages)


def test_scpi_power_commands_and_query():
    seen: list[bytes] = []

    def handler(conn):
        data = conn.recv(100)
        seen.append(data)
        if data.startswith(b"OUTP?"):
            conn.sendall(b"ON\n")

    srv, port = _serve_once(handler)
    try:
        p = bench.ScpiPower("127.0.0.1", port, 2)
        p.cycle(0)
        assert p.is_on() is True
    finally:
        srv.close()
    assert seen == [b"OUTP CH2,OFF\n", b"OUTP CH2,ON\n", b"OUTP? CH2\n"]


def test_scpi_power_unreachable_is_bench_error():
    def refuse(addr, timeout):
        raise ConnectionRefusedError("refused")

    with pytest.raises(BenchError, match="OUTP CH1,ON"):
        bench.ScpiPower("h", 1, 1, connect=refuse).on()


def _cp(rc=0, out="", err=""):
    return subprocess.CompletedProcess([], rc, out, err)


def test_labgrid_power():
    calls = []

    def runner(argv, **kw):
        calls.append(argv)
        return _cp(out="power for place p1 is off\n") if argv[-1] == "get" else _cp()

    p = bench.LabgridPower("p1", runner=runner)
    p.on()
    assert p.is_on() is False
    assert calls[0] == ["labgrid-client", "-p", "p1", "power", "on"]
    with pytest.raises(BenchError, match="not acquired"):
        bench.LabgridPower(
            "p1", runner=lambda a, **k: _cp(1, err="place not acquired")
        ).off()


def test_fake_power_records_events():
    fed = []
    p = FakePower(on_hook=lambda: fed.append(1))
    p.cycle()
    assert p.events == ["off", "on"] and fed == [1] and p.is_on()


# ---------------------------------------------------------------- Probe


class JLinkRunner:
    """Fake JLinkExe: records argv + command script, emulates savebin."""

    def __init__(self, rc=0, out="Found SW-DP with ID 0x0BE12477\nO.K.\n", mem=b""):
        self.rc, self.out, self.mem = rc, out, mem
        self.argv: list[list[str]] = []
        self.scripts: list[list[str]] = []

    def __call__(self, argv, **kw):
        self.argv.append(argv)
        script = Path(argv[argv.index("-CommanderScript") + 1]).read_text(encoding="utf-8").splitlines()
        self.scripts.append(script)
        for line in script:
            if m := re.match(r"savebin (\S+),0x([0-9a-f]+),0x([0-9a-f]+)", line):
                Path(m.group(1)).write_bytes(self.mem[: int(m.group(3), 16)])
        return _cp(self.rc, self.out)


def test_jlink_session_shape_and_guard(tmp_path):
    r = JLinkRunner()
    p = bench.JLinkProbe("123456", runner=r)
    img = tmp_path / "boot.bin"
    img.write_bytes(b"\0" * 16)
    p.loadbin(img, 0x08000000)
    argv, script = r.argv[0], r.scripts[0]
    assert argv[argv.index("-SelectEmuBySN") + 1] == "123456"
    assert "-autoconnect" not in argv
    assert script[:2] == [
        "exec DisableAutoUpdateFW",
        "connect",
    ]  # guard BEFORE the probe opens
    assert f"loadbin {img},0x8000000" in script and script[-1] == "exit"


def test_jlink_dp_id_reads_even_when_connect_fails():
    r = JLinkRunner(
        rc=1, out="Found SW-DP with ID 0x6BA02477\nCannot connect to target.\n"
    )
    assert bench.JLinkProbe("1", runner=r).dp_id() == 0x6BA02477
    r2 = JLinkRunner(out="DPIDR: 0x0BE12477\n")
    assert bench.JLinkProbe("1", runner=r2).dp_id() == 0x0BE12477
    with pytest.raises(BenchError, match="no DP ID"):
        bench.JLinkProbe("1", runner=JLinkRunner(out="nothing\n")).dp_id()


def test_jlink_failure_is_bench_error(tmp_path):
    img = tmp_path / "a.bin"
    img.write_bytes(b"x")
    with pytest.raises(BenchError):
        bench.JLinkProbe("1", runner=JLinkRunner(rc=1)).loadbin(img, 0)
    with pytest.raises(BenchError):
        bench.JLinkProbe(
            "1", runner=JLinkRunner(out="****** Error: Verify failed\n")
        ).loadbin(img, 0)
    with pytest.raises(ValueError):
        bench.JLinkProbe("1", runner=JLinkRunner()).loadbin(
            tmp_path / "has space.bin", 0
        )


def test_jlink_savebin_fresh_session_and_size_check(tmp_path):
    r = JLinkRunner(mem=b"\xaa" * 64)
    p = bench.JLinkProbe("1", runner=r)
    out = tmp_path / "rb.bin"
    p.savebin(out, 0x08008000, 64)
    p.savebin(out, 0x08008000, 64)
    assert out.read_bytes() == b"\xaa" * 64
    assert len(r.argv) == 2  # one process per read
    with pytest.raises(BenchError, match="128-byte"):
        p.savebin(out, 0x08008000, 128)  # probe returned short data


def test_script_probe(tmp_path):
    calls = []

    def runner(argv, **kw):
        calls.append(argv)
        if argv[-1] == "dp-id":
            return _cp(out="dp 0x0BE12477\n")
        if argv[1] == "savebin":
            Path(argv[2]).write_bytes(b"\0" * int(argv[4], 16))
        return _cp()

    w = tmp_path / "gd32"
    p = bench.ScriptProbe(w, runner=runner)
    assert p.dp_id() == 0x0BE12477
    p.savebin(tmp_path / "o.bin", 0x0800A000, 0x10)
    p.reset_run()
    assert calls[1] == [str(w), "savebin", str(tmp_path / "o.bin"), "0x800a000", "0x10"]
    assert calls[2] == [str(w), "reset-run"]
    with pytest.raises(BenchError):
        bench.ScriptProbe(w, runner=lambda a, **k: _cp(2, err="no probe")).reset_run()


def test_fake_probe_overlays_memory(tmp_path):
    img = tmp_path / "i.bin"
    img.write_bytes(b"\x01\x02\x03\x04")
    p = FakeProbe(memory={0x100: b"\xaa\xbb"})
    p.loadbin(img, 0x200)
    out = tmp_path / "o.bin"
    p.savebin(out, 0x0FF, 8)
    assert out.read_bytes() == b"\xff\xaa\xbb" + b"\xff" * 5
    p.savebin(out, 0x202, 4)
    assert out.read_bytes() == b"\x03\x04\xff\xff"
    assert p.sessions == 3


# ---------------------------------------------------------------- bench.yaml

_YAML = """\
console: {kind: tcp, host: consolehost, port: 4001}
power: {kind: scpi, host: psuhost, port: 5025, channel: 2}
probe: {kind: script, wrapper: tools/gd32.sh}
linux: {user: root, host: null}
i2c_bus: {eeprom: 0, pmic: null, brd: 3}
scif:
  flash_writer: fw/writer.mot
  baud: 115200
  program_start: {bl2_mmc: null, fip: 0x1234}
"""


def test_load_bench(tmp_path):
    y = tmp_path / "bench.yaml"
    y.write_text(_YAML, encoding="utf-8")
    b = bench.load_bench(y, FakeOperator())
    assert isinstance(b.console, bench.TcpConsole) and b.console.port == 4001
    assert isinstance(b.power, bench.ScpiPower) and b.power.channel == 2
    assert (
        isinstance(b.probe, bench.ScriptProbe)
        and b.probe.wrapper == tmp_path / "tools/gd32.sh"
    )
    assert b.linux_user == "root" and b.linux_host is None
    assert b.i2c_bus == {"eeprom": 0, "pmic": None, "brd": 3}
    assert b.scif["flash_writer"] == tmp_path / "fw/writer.mot"
    assert b.scif["program_start"] == {"bl2_mmc": None, "fip": 0x1234}
    assert b.raw["console"]["host"] == "consolehost"


@pytest.mark.parametrize(
    "path",
    [
        "scif",
        "scif.program_start",
        "scif.program_start.fip",
        "i2c_bus.brd",
        "power",
        "console.kind",
    ],
)
def test_load_bench_names_missing_key(tmp_path, path):
    import yaml

    raw = yaml.safe_load(_YAML)
    *parents, leaf = path.split(".")
    d = raw
    for k in parents:
        d = d[k]
    del d[leaf]
    y = tmp_path / "bench.yaml"
    y.write_text(yaml.safe_dump(raw), encoding="utf-8")
    with pytest.raises(ValueError, match=re.escape(f"missing '{path}'")):
        bench.load_bench(y, FakeOperator())


def test_load_bench_rejects_bad_values(tmp_path):
    y = tmp_path / "bench.yaml"
    y.write_text(_YAML.replace("kind: tcp", "kind: telnet"), encoding="utf-8")
    with pytest.raises(ValueError, match="console.kind"):
        bench.load_bench(y)
    y.write_text(_YAML.replace("fip: 0x1234", "fip: TBD"), encoding="utf-8")
    with pytest.raises(ValueError, match="program_start.fip"):
        bench.load_bench(y)


def test_load_bench_serial_manual_jlink_open_nothing(tmp_path):
    y = tmp_path / "bench.yaml"
    y.write_text(
        _YAML.replace(
            "{kind: tcp, host: consolehost, port: 4001}",
            "{kind: serial, port: /dev/null-port, baud: 115200}",
        )
        .replace(
            "{kind: scpi, host: psuhost, port: 5025, channel: 2}", "{kind: manual}"
        )
        .replace(
            "{kind: script, wrapper: tools/gd32.sh}", "{kind: jlink, serial: '000123'}"
        ),
        encoding="utf-8",
    )
    b = bench.load_bench(y, FakeOperator())  # lazy: no port is opened here
    assert isinstance(b.console, bench.SerialConsole) and b.console._ser is None
    assert isinstance(b.power, bench.ManualPower)
    assert isinstance(b.probe, bench.JLinkProbe) and b.probe.serial_no == "000123"


# ---------------------------------------------------------------- FakeLinux


def test_fake_linux(tmp_path):
    pytest.importorskip("provision.linux_target")
    t = FakeLinux(
        {r"^cat /sys/block/mmcblk\d/device/type": "MMC\n", r"^false": (1, "", "boom")}
    )
    assert t.run("cat /sys/block/mmcblk0/device/type").stdout == "MMC\n"
    with pytest.raises(BenchError):
        t.run("false")
    assert t.run("false", check=False).rc == 1
    with pytest.raises(AssertionError):
        t.run("reboot")
    src = tmp_path / "f.bin"
    src.write_bytes(b"abcdef")
    t.put(src, "/tmp/f.bin")
    assert t.md5("/tmp/f.bin", 2, 2) == __import__("hashlib").md5(b"cd").hexdigest()
