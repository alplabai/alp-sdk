"""RTL8211F(I) power rules (datasheet Rev 1.7 Table 53 notes 1-2): the provisioning
tool must never short-dwell OFF, blip ON, or power-cycle twice in a row."""
import pytest
from provision import bench, scif_writer
from .provision_fakes import FakeConsole


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class _Sock:
    def __init__(self, psu, cmd=None):
        self.psu = psu

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def close(self):
        pass

    def sendall(self, data):
        cmd = data.decode().strip()
        self.psu.events.append((self.psu.clock(), cmd))
        if cmd.startswith("OUTP CH1,"):
            self.psu.on = cmd.endswith("ON")

    def recv(self, n):
        return b"0x14" + bytes([10]) if self.psu.on else b"0x4" + bytes([10])


class FakePsu:
    def __init__(self):
        self.clock = Clock()
        self.events = []
        self.on = False

    def power(self):
        p = bench.ScpiPower("h", 1, 1, connect=lambda *a, **k: _Sock(self))
        p._clock, p._sleep = self.clock, self.clock.sleep
        return p

    def at(self, what):
        return [t for t, c in self.events if c.endswith(what)]


def test_yaml_off_s_below_minimum_clamped_up():
    b = bench.Bench(None, None, None, None, "root", None, {}, {}, {"power": {"off_s": 3}})
    assert b.off_s == bench.MIN_OFF_S
    b.raw["power"]["off_s"] = 20
    assert b.off_s == 20.0


def test_cycle_dwell_at_least_min_off_even_if_asked_for_3s():
    psu = FakePsu()
    p = psu.power()
    p.cycle(3)
    (off,), on = psu.at("OFF"), psu.at("ON")
    assert on[0] - off >= bench.MIN_OFF_S


def test_cycle_right_after_on_waits_min_on():
    psu = FakePsu()
    p = psu.power()
    p.on()
    psu.clock.sleep(0.5)
    p.cycle(15)
    assert psu.at("OFF")[0] - psu.at("ON")[0] >= bench.MIN_ON_S
    assert any("min ON" in line for line in p.log)
    assert all("[t=" in line for line in p.log)   # every command timestamped


def test_parameter_error_banner_refuses():
    con = FakeConsole([(None, "SCI Download mode (Due to parameter error)\r\n")])
    with pytest.raises(bench.BenchError, match="no valid image"):
        scif_writer.load_writer(con, __file__, 0.1)


# ---- the ROM banner must survive the power cycle, whenever it lands ----------

BANNER = "SCI Download mode (Normal SCI boot)\r\n-- Load Program to SRAM ---------------\r\n\r\n>"


class _RomConsole(FakeConsole):
    """Emits BANNER when the PSU's ON arrives: at once, or only after
    ``late_reads`` empty reads (the banner landing after the ON edge)."""

    def __init__(self, late_reads=0):
        super().__init__([])
        self.late_reads, self._armed, self._n = late_reads, False, 0

    def pump(self, seconds):  # fake time: one read, no real dwell
        self._pull(0)

    def arm(self):
        self._armed = True

    def _read_raw(self, timeout):
        if self._armed:
            self._n += 1
            if self._n > self.late_reads:
                self._armed = False
                self.feed(BANNER)
        return super()._read_raw(timeout)


def _rig(late_reads):
    from .test_provision_steps import _bench

    psu, con = FakePsu(), _RomConsole(late_reads)
    p = psu.power()
    orig = p.on
    p.on = lambda: (orig(), con.arm())
    b = _bench(console=con)
    b.power = p
    return b


@pytest.mark.parametrize("late", [0, 50])
def test_detect_sees_a_banner_that_lands_at_or_after_the_on_edge(tmp_path, late):
    from provision import steps
    from .test_provision_steps import _ctx

    ctx = _ctx(tmp_path, bench=_rig(late), execute=True)
    res = steps.run_one(steps.Detect(), ctx)
    assert res.status == "done" and ctx.boot_class == "scif-rom"
    assert "Normal SCI boot" in ctx.step_logs["detect"]       # console transcript persisted


@pytest.mark.parametrize("late", [0, 50])
def test_bootstrap_alone_sees_a_banner_that_lands_at_or_after_the_on_edge(tmp_path, monkeypatch, late):
    from provision import steps
    from .test_provision_steps import _ctx

    for fn in ("_stream", "em_w", "em_secsd", "em_dcid"):
        monkeypatch.setattr(scif_writer, fn, lambda *a, **k: None)
    monkeypatch.setattr(steps.Ctx, "artefact_bytes", lambda self, k: b"x")
    b = _rig(late)
    (tmp_path / "w.mot").write_bytes(b"S0030000FC")
    b.scif["flash_writer"] = tmp_path / "w.mot"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    res = steps.run_one(steps.Bootstrap(), ctx, force=True)
    assert res.status == "done", res.detail
    assert b.power.log and "Normal SCI boot" in ctx.step_logs["bootstrap"]


# ---- review round: restarts, Manual/Labgrid, failure paths ----------------------

def test_fresh_power_first_off_waits_min_on():
    psu = FakePsu()
    p = psu.power()
    t0 = psu.clock()
    p.off()                       # a new process right after another run's ON
    assert psu.at("OFF")[0] - t0 >= bench.MIN_ON_S


def test_labgrid_min_on_and_on_recorded_even_if_command_fails():
    clk = Clock()
    calls = []

    def runner(argv, **k):
        calls.append((clk(), argv[-1]))
        from types import SimpleNamespace
        return SimpleNamespace(returncode=0, stdout="", stderr="")
    p = bench.LabgridPower("pl", runner=runner)
    p._clock, p._sleep = clk, clk.sleep
    p.on()
    p.off()
    assert calls[1][0] - calls[0][0] >= bench.MIN_ON_S

    q = bench.LabgridPower("pl", runner=lambda *a, **k: (_ for _ in ()).throw(OSError("x")))
    q._clock, q._sleep = clk, clk.sleep
    with pytest.raises(bench.BenchError):
        q.on()
    assert q.on_count == 1 and q._last_on is not None


def test_manual_power_min_on_and_marks_on_after_confirm():
    from .provision_fakes import FakeOperator
    clk = Clock()
    seen = []

    class Op(FakeOperator):
        def confirm(self, m):
            seen.append(p.on_count)

    p = bench.ManualPower(Op())
    p._clock, p._sleep = clk, clk.sleep
    p.cycle(12)
    assert seen == [0] and p.on_count == 1       # marked after the operator confirmed
    t = clk()
    p.cycle(12)
    assert clk() - t >= bench.MIN_ON_S


def test_an_on_between_detect_and_bootstrap_forces_a_cycle(tmp_path, monkeypatch):
    from provision import steps
    from .test_provision_steps import _bench, _ctx

    for fn in ("_stream", "em_w", "em_secsd", "em_dcid"):
        monkeypatch.setattr(scif_writer, fn, lambda *a, **k: None)
    monkeypatch.setattr(steps.Ctx, "artefact_bytes", lambda self, k: b"x")
    b = _bench(console=FakeConsole([(None, BANNER)]))
    (tmp_path / "w.mot").write_bytes(b"S0030000FC")
    b.scif["flash_writer"] = tmp_path / "w.mot"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    steps.Detect().run(ctx)
    assert b.power.events == ["off", "on"]
    b.power.on()                                  # something powered the unit again
    b.power.on_hook = lambda: b.console.feed(BANNER)   # the forced cycle's ON prints the banner
    steps.Bootstrap().run(ctx)
    assert b.power.events == ["off", "on", "on", "off", "on"]
    assert ctx.rom_live_on_count is None


def test_failure_log_write_error_is_reported_not_swallowed(tmp_path, monkeypatch):
    from provision import ledger_out, steps
    from .test_provision_steps import _bench, _ctx

    def boom(*a, **k):
        raise OSError("disk full")
    monkeypatch.setattr(ledger_out, "write_log", boom)
    b = _bench(console=FakeConsole([(None, "SCI Download mode (Due to parameter error)\r\n")]))
    ctx = _ctx(tmp_path, bench=b, execute=True)
    res = steps.run_one(steps.Detect(), ctx)
    assert res.status == "failed" and any("disk full" in c for c in res.commands)
    assert any("disk full" in line for line in ctx.plan_log)


def test_step_log_elides_the_base64_push_echo():
    from provision import steps
    text = "start\n" + "\n".join("A" * 1024 for _ in range(50)) + "\nend"
    out = steps._elide_long_lines(text)
    assert len(out) < 300 and "48 long lines" in out and "start" in out and out.endswith("end")


def test_step_log_is_written_to_the_ledger_on_success_too(tmp_path):
    from provision import steps
    from .test_provision_steps import _ctx

    ctx = _ctx(tmp_path, bench=_rig(0), execute=True)
    assert steps.run_one(steps.Detect(), ctx).status == "done"
    logs = list((ctx.ledger_root / ctx.sku / "logs" / ctx.serial).glob("detect-*.log"))
    assert len(logs) == 1
    text = logs[0].read_text(encoding="utf-8")
    assert "POWER SCPI" in text and "Normal SCI boot" in text


# ---- boot_sd_linux continues dsw1_emmc_insert_sd's boot; gd32_flash dry run ------

def test_dsw1_emmc_insert_sd_then_boot_sd_linux_cycles_exactly_once(tmp_path, monkeypatch):
    from provision import linux_target as lt, steps
    from .test_provision_steps import _bench, _ctx

    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    monkeypatch.setattr(steps, "connect_linux", lambda ctx, force=False, **kw: (_ for _ in ()).throw(bench.BenchError("no ip")))
    monkeypatch.setattr(steps, "_phy_latch_evidence", lambda c: "")      # no PHY fault: only the DHCP wait runs
    monkeypatch.setattr(steps, "IP_WAIT_S", 0.05)
    monkeypatch.setattr(steps.time, "sleep", lambda s: None)
    monkeypatch.setattr(steps, "clean_shutdown", lambda ctx: None)    # covered in test_provision_clean_shutdown
    b = _bench(console=FakeConsole([]))
    b.power.on_hook = lambda: b.console.feed("Hit any key to stop autoboot: 3\r\nlogin: ")
    ctx = _ctx(tmp_path, bench=b, execute=True, gd32_fw=tmp_path)   # pending GD32 flash: console path after the wait
    assert steps.OpDsw1EmmcInsertSd().run(ctx).status == "done"
    def stop(*a, **k):                       # everything after the boot is out of scope here
        raise steps.Refused("stop-after-boot")
    monkeypatch.setattr(steps, "tier_gate", stop)
    with pytest.raises(steps.Refused, match="stop-after-boot"):
        steps.BootSdLinux().run(ctx)
    assert b.power.events == ["off", "on"]
    assert ctx.live_boot is None
    # an ON since then makes the live boot stale: the next boot cycles again
    ctx.live_boot = (b.power.on_count - 1, 0)
    b.power.on_hook = lambda: b.console.feed("login: ")
    with pytest.raises(steps.Refused, match="stop-after-boot"):
        steps.BootSdLinux().run(ctx)
    assert b.power.events == ["off", "on", "off", "on"]


def test_gd32_flash_dry_run_plans_console_transport_without_the_probe(tmp_path, monkeypatch):
    from provision import steps
    from .test_provision_steps import _bench, _ctx, _gd32_fw

    class Boom:
        def __getattr__(self, n):
            raise AssertionError(f"probe.{n} invoked in a dry run")
    b = _bench()
    b.probe = Boom()
    ctx = _ctx(tmp_path, bench=b, execute=False, gd32_fw=_gd32_fw(tmp_path))
    monkeypatch.setattr(type(ctx), "linux_up", lambda self: False)
    res = steps.Gd32Flash().run(ctx)
    assert res.status != "failed" and res.evidence["gd32_flash_transport"].startswith("console")
    assert any("WOULD: no reachable IP" in c for c in ctx.plan_log)
    assert any("WOULD: loadbin" in c for c in ctx.plan_log)


# ---- console-only (GD32 flash pending) boot_sd_linux stays done; probe flips keep their log ----

def _console_only_ctx(tmp_path, monkeypatch):
    from types import SimpleNamespace
    from provision import linux_target as lt, steps
    from .test_provision_steps import _bench, _ctx

    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    monkeypatch.setattr(lt, "root_device", lambda t: "mmcblk1p2")
    monkeypatch.setattr(lt, "resolve_emmc", lambda t: "mmcblk0")
    monkeypatch.setattr(steps, "connect_linux", lambda ctx, force=False, **kw: (_ for _ in ()).throw(bench.BenchError("no ip")))
    monkeypatch.setattr(steps, "som_presence_problems", lambda ctx, t: [])
    monkeypatch.setattr(steps, "_phy_latch_evidence", lambda c: "")      # no PHY fault: only the DHCP wait runs
    monkeypatch.setattr(steps, "IP_WAIT_S", 0.05)
    monkeypatch.setattr(steps.time, "sleep", lambda s: None)
    monkeypatch.setattr(steps, "tier_gate", lambda ctx, mib: (SimpleNamespace(ok=True, detail=""), {}))
    b = _bench(console=FakeConsole([]))
    b.power.on_hook = lambda: b.console.feed("Hit any key to stop autoboot: 3\r\nlogin: ")
    return b, _ctx(tmp_path, bench=b, execute=True, gd32_fw=tmp_path)   # its GD32 flash is pending


def test_console_only_boot_sd_linux_stays_done_and_gd32_flash_goes_over_the_console(tmp_path, monkeypatch):
    from provision import steps

    b, ctx = _console_only_ctx(tmp_path, monkeypatch)
    res = steps.run_one(steps.BootSdLinux(), ctx, force=True)
    assert res.status == "done", res.detail
    assert "no network yet" in res.detail
    assert isinstance(steps.BootSdLinux().probe(ctx), steps.Satisfied)
    # the next step picks the console transport (no IP)
    tools = tmp_path / "tools"
    tools.mkdir()
    (tools / "a.py").write_text("x", encoding="utf-8")
    b.console_swd = (tools, ("a.py",))
    monkeypatch.setattr(type(ctx), "_check_unit_identity", lambda self, t: None)
    t, probe, via_console = steps.Gd32Flash._transport(ctx)
    assert via_console and t is not None
    # a later ON makes the console state stale
    b.power.on()
    assert not isinstance(steps.BootSdLinux().probe(ctx), steps.Satisfied)


def test_post_run_probe_flip_still_writes_the_step_log(tmp_path):
    from provision import steps
    from .test_provision_steps import _bench, _ctx

    class Flip(steps.Step):
        name = "flipper"

        def run(self, ctx):
            return self.result(ctx, "ran")

        def probe(self, ctx):
            return steps.Unsatisfied("still not so")

    b = _bench(console=FakeConsole([(None, "boot text here\r\n")]))
    b.console.pump(0)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    res = steps.run_one(Flip(), ctx, force=True)
    assert res.status == "failed" and "post-run probe" in res.detail
    logs = list((ctx.ledger_root / ctx.sku / "logs" / ctx.serial).glob("flipper-*.log"))
    assert len(logs) == 1 and "post-run probe flipped" in logs[0].read_text(encoding="utf-8")


def test_discovered_host_is_exported_to_the_probe_wrapper(tmp_path):
    from provision import steps
    from .test_provision_steps import _bench, _ctx

    seen = []

    def runner(argv, **kw):
        from types import SimpleNamespace
        seen.append(kw.get("env", {}).get("ALP_PROVISION_HOST"))
        return SimpleNamespace(returncode=0, stdout="0x0be12477", stderr="")
    b = _bench()
    b.probe = bench.ScriptProbe(tmp_path / "w.sh", runner=runner)
    ctx = _ctx(tmp_path, bench=b)
    ctx.attach_linux("192.168.1.251")
    b.probe.dp_id()
    assert seen == ["192.168.1.251"]
