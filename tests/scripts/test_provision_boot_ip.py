"""boot_sd_linux: a console login without a host gets one extra cold cycle on the end0
PHY-latch signature (#2582) or a bounded DHCP wait, and fails clearly otherwise;
need_linux() names the real reason when no Linux target is attached."""

from __future__ import annotations

import pytest
from provision import steps
from provision import linux_target as lt
from provision.bench import BenchError

from .provision_fakes import FakeConsole, FakeLinux, FakePower
from .test_provision_steps import Board, _bench, _ctx

LATCH = "renesas-gbeth 15c30000.ethernet end0: Failed to reset the dma"
WHY = "gbeth DMA reset failed on end0"


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    monkeypatch.setattr(steps.time, "sleep", lambda s: None)
    monkeypatch.setattr(steps, "IP_WAIT_S", 0.05)
    monkeypatch.setattr(steps, "IP_POLL_S", 0.01)


def _console_bench():
    console = FakeConsole([])
    b = _bench(console=console)
    b.power = FakePower(on_hook=lambda: console.feed("login: "))
    return b


def _connect_after(board, failures):
    """connect_linux stand-in: no host for `failures` calls (DHCP not up yet), then `board`."""
    calls = []

    def connect(c, force=False, rediscover=False):
        calls.append(1)
        if len(calls) <= failures:
            raise BenchError("no inet address on end0")
        c.linux = board
    return connect, calls


# --- boot_to_linux ----------------------------------------------------------------------

def test_boot_to_linux_keeps_polling_until_dhcp_answers(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True)
    board = Board()
    connect, calls = _connect_after(board, failures=3)        # first look + 2 polls fail
    monkeypatch.setattr(steps, "connect_linux", connect)
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    steps.boot_to_linux(ctx, need_ip=False, ip_wait_s=30.0)
    assert ctx.linux is board and len(calls) == 4


def test_boot_to_linux_without_a_wait_looks_once(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True)
    connect, calls = _connect_after(Board(), failures=99)
    monkeypatch.setattr(steps, "connect_linux", connect)
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    steps.boot_to_linux(ctx, need_ip=False)
    assert ctx.linux is None and len(calls) == 1


# --- the PHY-latch signature -------------------------------------------------------------

def _evidence_for(monkeypatch, tmp_path, dmesg, ifaces="end0\nend1\n", carrier="0\n"):
    fake = FakeLinux({r"^dmesg \| grep": dmesg, r"^ls /sys/class/net": ifaces,
                      r"^cat /sys/class/net/end0/carrier": carrier})
    monkeypatch.setattr(steps, "ConsoleTarget", lambda console: fake)
    return steps._phy_latch_evidence(_ctx(tmp_path, bench=_console_bench(), execute=True))


def test_latch_evidence_names_the_ports_from_the_stmmac_dmesg_lines(tmp_path, monkeypatch):
    assert _evidence_for(monkeypatch, tmp_path / "a", LATCH + "\n") == WHY
    end1 = LATCH.replace("end0", "end1").replace("Failed to reset the dma", "Hw setup failed")
    assert _evidence_for(monkeypatch, tmp_path / "b", LATCH + "\n" + end1 + "\n") == \
        "gbeth DMA reset failed on end0, end1"
    assert _evidence_for(monkeypatch, tmp_path / "c", "stmmac: DMA engine initialization failed\n") == \
        "gbeth DMA reset failed on unknown ports"


def test_latch_evidence_falls_back_to_no_carrier(tmp_path, monkeypatch):
    assert _evidence_for(monkeypatch, tmp_path, "") == "no carrier"


def test_no_latch_evidence_when_end0_has_carrier(tmp_path, monkeypatch):
    assert _evidence_for(monkeypatch, tmp_path, "", carrier="1\n") == ""


def _tail(tmp_path, monkeypatch, evidence, boots, gd32_fw=None):
    """_net_after_boot with the evidence faked and boot_to_linux replaced by `boots`
    (one entry per extra cold cycle: the Board that answers, or None)."""
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True, gd32_fw=gd32_fw)
    cycles = []

    def boot(c, need_ip=True, ip_wait_s=0.0, **kw):
        cycles.append(ip_wait_s)
        c.linux = boots.pop(0)
        return "second boot"
    monkeypatch.setattr(steps, "boot_to_linux", boot)
    monkeypatch.setattr(steps, "_phy_latch_evidence", lambda c: evidence)
    return ctx, cycles


def test_phy_latch_gets_exactly_one_extra_cold_cycle(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, LATCH, [Board()])
    ev = {}
    assert steps._net_after_boot(ctx, ev, "first boot") == "second boot"
    assert ctx.linux is not None and len(cycles) == 1 and ev["end0_no_carrier_retries"] == "1"
    assert any("#2582" in line for line in ctx.plan_log)


def test_phy_latch_that_persists_fails_naming_2582_and_the_dmesg_line(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, LATCH, [None])
    with pytest.raises(BenchError) as e:
        steps._net_after_boot(ctx, {}, "first boot")
    assert "#2582" in str(e.value) and LATCH in str(e.value) and len(cycles) == 1   # never a second retry


def test_a_pending_gd32_flash_does_not_suppress_the_retry_and_the_cause_is_recorded(tmp_path, monkeypatch):
    # 2026W38-0005 booted with a blank GD32 and had Ethernet: no GD32 state excuses a dead gbeth
    both = "gbeth DMA reset failed on end0, end1"
    ctx, cycles = _tail(tmp_path, monkeypatch, both, [None], gd32_fw=tmp_path)
    ev = {}
    assert steps._net_after_boot(ctx, ev, "first boot") == "second boot"
    assert ctx.linux is None and len(cycles) == 1 and ev["end0_no_carrier_retries"] == "1"
    assert ev["network"] == f"none ({both})"


def test_a_pending_gd32_flash_with_no_carrier_records_it(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, "no carrier", [None], gd32_fw=tmp_path)
    ev = {}
    steps._net_after_boot(ctx, ev, "first boot")
    assert len(cycles) == 1 and ev["network"] == "none (no carrier)"


def test_a_pending_gd32_flash_whose_retry_recovers_has_a_network(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, WHY, [Board()], gd32_fw=tmp_path)
    ev = {}
    steps._net_after_boot(ctx, ev, "first boot")
    assert ctx.linux is not None and len(cycles) == 1 and "network" not in ev


def test_extra_cold_cycle_goes_through_power_cycle_with_min_off(tmp_path):
    """Not a bare off/on: boot_to_linux cycles via Power.cycle(off_s), which clamps to MIN_OFF_S."""
    from provision.bench import MIN_OFF_S, Power
    seen = []

    class P(Power):
        def off(self): seen.append("off")
        def on(self): seen.append("on")
    p = P()
    p._sleep = lambda s: seen.append(f"sleep {s:g}")
    p.cycle(0.1)
    assert seen[0] == "off" and "on" in seen and any(x == f"sleep {MIN_OFF_S:g}" for x in seen)


def test_without_the_signature_a_slow_lease_is_waited_for(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, "", [])
    board = Board()
    connect, calls = _connect_after(board, failures=2)
    monkeypatch.setattr(steps, "connect_linux", connect)
    steps._net_after_boot(ctx, {}, "first boot")
    assert ctx.linux is board and cycles == []                # no extra power cycle


def test_a_pending_gd32_flash_still_waits_for_a_slow_lease(tmp_path, monkeypatch):
    ctx, cycles = _tail(tmp_path, monkeypatch, "", [], gd32_fw=tmp_path)
    board = Board()
    connect, _ = _connect_after(board, failures=2)
    monkeypatch.setattr(steps, "connect_linux", connect)
    steps._net_after_boot(ctx, {}, "first boot")
    assert ctx.linux is board and cycles == []


# --- the step ----------------------------------------------------------------------------

def _step(tmp_path, monkeypatch, evidence, second):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True)
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    monkeypatch.setattr(steps, "_phy_latch_evidence", lambda c: evidence)
    monkeypatch.setattr(steps, "connect_linux",
                        lambda c, force=False, rediscover=False: (_ for _ in ()).throw(BenchError("no inet")))
    return ctx, steps.run_steps(ctx, only=["boot_sd_linux"])[-1]


def test_boot_sd_linux_fails_when_the_latch_survives_the_extra_cold_cycle(tmp_path, monkeypatch):
    ctx, r = _step(tmp_path, monkeypatch, LATCH, None)
    assert r.status == "failed" and "#2582" in r.detail and LATCH in r.detail
    assert ctx.linux is None and ctx.bench.power.events == ["off", "on", "off", "on"]   # first + one retry


def test_boot_sd_linux_with_a_pending_gd32_flash_continues_on_the_console_and_records_why(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True, gd32_fw=tmp_path)
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)
    monkeypatch.setattr(lt, "root_device", lambda t: "mmcblk1p2")
    monkeypatch.setattr(lt, "resolve_emmc", lambda t: "mmcblk0")
    monkeypatch.setattr(steps, "som_presence_problems", lambda c, t=None: [])
    monkeypatch.setattr(steps, "tier_gate", lambda c, mib=None: (type("T", (), {"ok": True, "detail": ""})(), {}))
    monkeypatch.setattr(steps, "_phy_latch_evidence", lambda c: "no carrier")
    monkeypatch.setattr(steps, "connect_linux",
                        lambda c, force=False, rediscover=False: (_ for _ in ()).throw(BenchError("no inet")))
    r = steps.run_one(steps.BootSdLinux(), ctx, force=True)
    assert r.status == "done", r.detail
    assert r.evidence["network"] == "none (no carrier)" and r.evidence["end0_no_carrier_retries"] == "1"
    assert ctx.bench.power.events == ["off", "on", "off", "on"]       # the retry ran despite the pending flash


def test_boot_sd_linux_fails_clearly_when_no_ipv4_appears(tmp_path, monkeypatch):
    ctx, r = _step(tmp_path, monkeypatch, "", None)
    assert r.status == "failed"
    assert r.detail == "Linux up on console but no IPv4 on end0 after 0.05s; check cable/DHCP"
    assert ctx.linux is None and ctx.console_linux_on_count is None
    assert ctx.bench.power.events == ["off", "on"]            # no retry cycle without the signature


# --- need_linux --------------------------------------------------------------------------

def test_need_linux_picks_up_a_lease_that_arrived_after_boot_sd_linux(tmp_path, monkeypatch):
    b = _console_bench()
    ctx = _ctx(tmp_path, bench=b, execute=True)
    ctx.console_linux_on_count = b.power.on_count
    board = Board()
    connect, _ = _connect_after(board, failures=0)
    monkeypatch.setattr(steps, "connect_linux", connect)
    assert ctx.need_linux() is board


def test_need_linux_error_is_accurate_after_a_console_only_boot(tmp_path, monkeypatch):
    b = _console_bench()
    ctx = _ctx(tmp_path, bench=b, execute=True)
    ctx.console_linux_on_count = b.power.on_count
    connect, _ = _connect_after(Board(), failures=99)
    monkeypatch.setattr(steps, "connect_linux", connect)
    with pytest.raises(steps.Refused) as e:
        ctx.need_linux()
    assert "up on the console but has no reachable IPv4" in str(e.value)
    assert "has not run" not in str(e.value)


def test_write_rootfs_without_a_target_names_the_real_cause(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)          # nothing ran, nothing pinned
    res = steps.run_steps(ctx, only=["write_rootfs"])[-1]
    assert res.status == "failed" and "no Linux target attached" in res.detail
    assert "has not run" not in res.detail
