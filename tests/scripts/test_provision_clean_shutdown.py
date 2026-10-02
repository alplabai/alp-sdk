"""Never hard-cut power under a running Linux (#2624): clean_shutdown() before every tool-driven
power cycle, and the census supply-current screen. Fakes only, no bench."""

from __future__ import annotations

import pytest
from provision import linux_target as lt
from provision import steps
from provision.bench import BenchError

from .provision_fakes import FakeConsole, FakeLinux, FakePower
from .test_provision_steps import Board, _bench, _ctx

HALT = "reboot: Power down\r\n"


class OrderedPower(FakePower):
    """FakePower that logs into the shared `order` list, so a test can see poweroff come before off."""

    def __init__(self, order, on_hook=None):
        super().__init__(on_hook=on_hook)
        self.order = order

    def off(self):
        self.order.append("psu-off")
        super().off()


class Unit(FakeLinux):
    """A Linux target that, on `poweroff`, prints (or not) the halt line on the console."""

    def __init__(self, console, order, halts=True):
        super().__init__({"^true$": "", "^sync$": ""})
        self.console, self.order, self.halts = console, order, halts

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None, long_running=False):
        self.order.append(cmd)
        if cmd == "sync; poweroff":
            if self.halts:
                self.console.feed(HALT if self.halts is True else self.halts)
            return lt.CmdResult(0, "", "")
        return super().run(cmd, timeout, check)


def _setup(tmp_path, halts=True, linux=True):
    order: list[str] = []
    console = FakeConsole([])
    b = _bench(console=console)
    b.power = OrderedPower(order, on_hook=lambda: console.feed("login: "))
    ctx = _ctx(tmp_path, bench=b, execute=True)
    if linux:
        ctx.linux = Unit(console, order, halts)
    return ctx, order


def test_poweroff_and_the_halt_line_come_before_the_psu_is_cut(tmp_path):
    ctx, order = _setup(tmp_path)
    steps.clean_shutdown(ctx)
    ctx.bench.power.cycle(15.0, ctx.bench.console)
    assert order == ["true", "sync; poweroff", "psu-off"]
    assert ctx.plan_log == ["clean shutdown: poweroff, halt line seen"]


def test_system_halted_is_also_a_halt_line(tmp_path):
    ctx, order = _setup(tmp_path, halts="System halted.")
    steps.clean_shutdown(ctx)
    assert ctx.plan_log == ["clean shutdown: poweroff, halt line seen"]


def test_no_halt_line_falls_back_to_sync_and_a_wait_and_says_so(tmp_path):
    ctx, order = _setup(tmp_path, halts=False)
    steps.clean_shutdown(ctx)
    assert order == ["true", "sync; poweroff", "sync"]
    assert len(ctx.plan_log) == 1 and ctx.plan_log[0].startswith("clean shutdown FALLBACK: no halt line in ")
    assert "sync + " in ctx.plan_log[0]


def test_a_unit_that_is_not_up_is_not_asked_to_power_off(tmp_path):
    ctx, order = _setup(tmp_path, linux=False)
    steps.clean_shutdown(ctx)
    assert order == [] and ctx.plan_log == []


def test_a_dead_ssh_link_is_not_reachable_linux(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.linux.responses["^true$"] = (255, "")
    steps.clean_shutdown(ctx)
    assert "sync; poweroff" not in order


def test_a_dry_run_never_touches_the_unit(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.execute = False
    steps.clean_shutdown(ctx)
    assert order == []


def test_a_console_only_unit_is_halted_through_the_console_shell(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path, linux=False)
    ctx.console_login_on_count = ctx.bench.power.on_count
    shell = Unit(ctx.bench.console, order)
    monkeypatch.setattr(steps, "ConsoleTarget", lambda console: shell)
    steps.clean_shutdown(ctx)
    assert order == ["sync; poweroff"] and ctx.plan_log == ["clean shutdown: poweroff, halt line seen"]


def test_a_console_login_from_an_earlier_boot_is_stale(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path, linux=False)
    ctx.console_login_on_count = ctx.bench.power.on_count - 1      # an ON happened since
    monkeypatch.setattr(steps, "ConsoleTarget", lambda console: pytest.fail("stale login used"))
    steps.clean_shutdown(ctx)
    assert order == []


def test_a_pinned_host_from_a_previous_run_is_halted_too(tmp_path, monkeypatch):
    """Detect starts a fresh process on a board that is still up: the pinned bench.yaml host
    is the only way to know, and the hard cut it used to get is what corrupted the SD root."""
    ctx, order = _setup(tmp_path, linux=False)
    ctx.bench.linux_host = "192.0.2.7"
    monkeypatch.setattr(lt, "LinuxTarget", lambda host, user: Unit(ctx.bench.console, order))
    steps.clean_shutdown(ctx)
    assert order == ["true", "sync; poweroff"]


def _boot_patches(monkeypatch, ctx, order):
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)

    def connect(c, force=False, rediscover=False):
        c.linux = Unit(c.bench.console, order)
    monkeypatch.setattr(steps, "connect_linux", connect)


def test_boot_to_linux_halts_before_every_cold_cycle_and_keeps_the_off_dwell(tmp_path, monkeypatch):
    """cold_boot_test = 3 x cold_boot_phy_retry -> boot_to_linux: three clean poweroffs, three cuts."""
    ctx, order = _setup(tmp_path)
    _boot_patches(monkeypatch, ctx, order)
    for _ in range(3):
        steps.boot_to_linux(ctx)
    assert order.count("sync; poweroff") == 3 and order.count("psu-off") == 3
    assert [o for o in order if o in ("sync; poweroff", "psu-off")] == ["sync; poweroff", "psu-off"] * 3
    assert ctx.bench.power.events == ["off", "on"] * 3


def test_resuming_a_running_boot_does_not_power_off_the_unit(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path)
    _boot_patches(monkeypatch, ctx, order)
    ctx.bench.console.feed("login: ")
    steps.boot_to_linux(ctx, resume_mark=0)
    assert order == [] and ctx.bench.power.events == []


def test_poweroff_and_cold_boot_is_a_clean_shutdown_then_a_cold_boot(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path)
    _boot_patches(monkeypatch, ctx, order)
    steps.poweroff_and_cold_boot(ctx)
    assert order[:3] == ["true", "sync; poweroff", "psu-off"] and ctx.bench.power.events == ["off", "on"]


def test_detect_halts_a_running_linux_before_the_power_cycle(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path)
    ctx.bench.power.on_hook = lambda: ctx.bench.console.feed("=> ")
    steps.Detect().run(ctx)
    assert order[:3] == ["true", "sync; poweroff", "psu-off"]


# --- census: supply-current screen ----------------------------------------------------------

class Metered(FakePower):
    def __init__(self, amps):
        super().__init__()
        self.amps, self.queries = amps, 0

    def current(self):
        self.queries += 1
        if isinstance(self.amps, Exception):
            raise self.amps
        return self.amps


def _census(tmp_path, monkeypatch, power):
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    ctx.bench.power = power
    ctx.linux = Board()
    monkeypatch.setattr(steps.lt, "census", lambda *a, **k: ({"cpu_khz": "1800000"}, []))
    return ctx, steps.Census().run(ctx)


def test_census_records_one_current_reading_and_its_state(tmp_path, monkeypatch):
    p = Metered(0.172)
    ctx, res = _census(tmp_path, monkeypatch, p)
    assert res.evidence["psu_current_a"] == "0.172" and res.evidence["psu_current_state"] == "linux-idle"
    assert p.queries == 1 and "WARNING" not in res.detail


def test_census_warns_above_0_40_a_but_does_not_fail(tmp_path, monkeypatch):
    ctx, res = _census(tmp_path, monkeypatch, Metered(0.451))
    assert res.status == "done" and "WARNING: supply current 0.451 A > 0.40 A" in res.detail
    assert res.evidence["psu_current_a"] == "0.451"


def test_census_does_not_warn_at_exactly_the_threshold(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, Metered(0.40))
    assert "WARNING" not in res.detail


def test_census_without_scpi_power_has_no_current_keys(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, FakePower())
    assert "psu_current_a" not in res.evidence and "psu_current_state" not in res.evidence


def test_census_records_a_failed_current_query_as_unread(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, Metered(BenchError("SCPI: still failing")))
    assert res.status == "done" and res.evidence["psu_current_a"].startswith("unread (")
    assert "psu_current_state" not in res.evidence


def test_census_queries_only_the_configured_channel(tmp_path, monkeypatch):
    from provision.bench import ScpiPower
    sent = []

    class Sock:
        def sendall(self, data):
            sent.append(data.decode().strip())

        def recv(self, n):
            return b"0.200" + bytes([10])

        def close(self):
            pass
    p = ScpiPower("h", 1, 1, connect=lambda addr, timeout: Sock())
    _, res = _census(tmp_path, monkeypatch, p)
    assert res.evidence["psu_current_a"] == "0.200"
    assert sent == ["MEAS:CURR? CH1"]


def test_pmic_verify_names_a_register_it_could_not_read_instead_of_passing(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), execute=True,
               expected_registers={"devices": {"act88760": {"addr": 0x25, "bus": "pmic", "registers": [
                   {"reg": 0x10, "expect": 0x08}]}}})
    ctx.linux = Board()
    ctx.linux.regs.clear()                  # every i2cget fails
    with pytest.raises(steps.Refused, match=r"act88760 0x25 reg 0x10 unread \(.*after 3 attempts"):
        steps.PmicVerify().run(ctx)
    assert ctx.linux.commands.count("i2cget -y -f 8 0x25 0x10") == 3
