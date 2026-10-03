# SPDX-License-Identifier: Apache-2.0
"""Provisioning throughput: --linux-host / console discovery fallback, SCPI pacing,
truthful dsw1 labels, gd32_flash probe without SWD, no extra cold cycle before functional_test."""

from __future__ import annotations

import pytest
from provision import bench, steps
from provision.bench import BenchError

from .provision_fakes import FakeConsole, FakePower, FakeProbe
from .test_provision_boot_ip import _connect_after, _console_bench
from .test_provision_bench import _Psu
from .test_provision_gd32_resume import _setup
from .test_provision_steps import Board, _bench, _ctx


# --- 1: linux host ---------------------------------------------------------------------

def test_need_linux_discovers_over_the_console_when_nothing_is_pinned(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True)
    board = Board()
    connect, calls = _connect_after(board, failures=0)
    monkeypatch.setattr(steps, "connect_linux", connect)
    monkeypatch.setattr(steps, "console_login_ctx", lambda c, timeout=None: None)
    assert ctx.need_linux() is board and len(calls) == 1


def test_need_linux_does_not_touch_the_console_when_a_host_is_pinned(tmp_path, monkeypatch):
    b = _console_bench()
    b.linux_host = "10.0.0.9"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    monkeypatch.setattr(steps, "console_login_ctx", lambda *a, **k: pytest.fail("console used"))
    assert ctx.need_linux().host == "10.0.0.9"


def test_need_linux_refusal_names_the_flag_when_discovery_fails(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_console_bench(), execute=True)
    connect, _ = _connect_after(Board(), failures=99)
    monkeypatch.setattr(steps, "connect_linux", connect)
    monkeypatch.setattr(steps, "console_login_ctx", lambda c, timeout=None: None)
    with pytest.raises(steps.Refused, match="--linux-host"):
        ctx.need_linux()


def test_linux_host_flag_is_parsed_for_run_and_plan():
    import provision_som
    for cmd in ("plan", "run"):
        a = provision_som._v2n_parser().parse_args([cmd, "--sku", "X", "--ledger-root", ".", "--bundle", ".", "--linux-host", "1.2.3.4"])
        assert a.linux_host == "1.2.3.4"


# --- 2: SCPI pacing --------------------------------------------------------------------

def test_scpi_gap_defaults_to_300ms_and_is_configurable():
    assert bench.ScpiPower("h", 1, 1).GAP_S == 0.3
    assert bench.ScpiPower("h", 1, 1, min_gap_s=0.5).GAP_S == 0.5
    with pytest.raises(BenchError):
        bench.ScpiPower("h", 1, 1, min_gap_s=-1)


def test_scpi_gap_is_enforced_with_the_configured_value():
    psu = _Psu()
    p = psu.power(1)
    p.GAP_S = 0.5
    p.is_on()
    t0 = p._clock()
    p.is_on()
    assert p._clock() - t0 >= 0.5 - 1e-9


def test_scpi_retries_after_a_connection_reset_with_backoff():
    psu = _Psu()
    p = psu.power(1)
    psu.reset_next = True
    assert p.is_on() is False
    assert psu.connects == 2


# --- 3: truthful label -----------------------------------------------------------------

@pytest.mark.parametrize("banner,want", [
    ("NOTICE:  BL2: SYS_LSI_MODE: 0x3c06\n", "xSPI"),
    ("NOTICE:  BL2: SYS_LSI_MODE: 0x3c01\n", "SYS_LSI_MODE 0x3c01"),
    ("", "boot mode not reported by BL2"),
])
def test_dsw1_label_reports_the_observed_boot_mode(tmp_path, banner, want):
    console = FakeConsole([])
    b = _bench(console=console)
    b.power = FakePower(on_hook=lambda: console.feed(banner + "Hit any key to stop autoboot"))
    ctx = _ctx(tmp_path, bench=b, execute=True)
    monkey_confirm = steps.OpDsw1EmmcInsertSd
    step = monkey_confirm()
    import unittest.mock as m
    with m.patch.object(steps, "clean_shutdown", return_value=None), \
            m.patch.object(steps.uboot, "AUTOBOOT", "autoboot"):
        res = step.run(ctx)
    assert want in res.detail and "eMMC" not in res.detail


# --- 4: gd32_flash probe without SWD ---------------------------------------------------

def _recorded(ctx, **ev):
    ctx.state.setdefault("steps", {})["gd32_flash"] = {"status": "done", "evidence": ev}


def test_gd32_probe_is_satisfied_by_bridge_and_ledger_without_swd(tmp_path):
    ctx, probe, _ = _setup(tmp_path)
    (ctx.gd32_fw / "VERSION").write_text("0.2.9\n", encoding="utf-8")
    _recorded(ctx, gd32_fw_version="0.2.9", gd32_protocol="GD32 bridge protocol 0.13.0")
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Satisfied) and "bridge GET_VERSION + ledger" in r.reason
    assert probe.calls == []                      # no dp_id, no savebin, nothing halts the core


@pytest.mark.parametrize("ledger", [
    {},                                                                    # no ledger version
    {"gd32_fw_version": "0.2.8", "gd32_protocol": "GD32 bridge protocol 0.13.0"},   # other release
    {"gd32_fw_version": "0.2.9", "gd32_protocol": "GD32 bridge protocol 0.12.0"},   # other protocol
])
def test_gd32_probe_falls_back_to_the_swd_readback(tmp_path, ledger):
    ctx, probe, _ = _setup(tmp_path)
    (ctx.gd32_fw / "VERSION").write_text("0.2.9\n", encoding="utf-8")
    if ledger:
        _recorded(ctx, **ledger)
    assert isinstance(steps.Gd32Flash().probe(ctx), steps.Satisfied)
    assert "savebin" in [c[0] for c in probe.calls]


def test_force_step_skips_the_bridge_shortcut(tmp_path):
    ctx, probe, _ = _setup(tmp_path)
    (ctx.gd32_fw / "VERSION").write_text("0.2.9\n", encoding="utf-8")
    _recorded(ctx, gd32_fw_version="0.2.9", gd32_protocol="GD32 bridge protocol 0.13.0")
    res = steps.run_steps(ctx, only=["gd32_flash"], force=["gd32_flash"])
    assert res[-1].status == "done" and "savebin" in [c[0] for c in probe.calls]


# --- 5: no extra cold cycle before functional_test -------------------------------------

def test_functional_test_never_cycles_power(tmp_path, monkeypatch):
    b = _bench()
    ctx = _ctx(tmp_path, bench=b, linux=Board(), execute=True)
    monkeypatch.setattr(steps.functest, "run", lambda *a, **k: ({}, "", 0.0))
    before = list(b.power.events)
    steps.FunctionalTest().run(ctx)
    assert b.power.events == before
