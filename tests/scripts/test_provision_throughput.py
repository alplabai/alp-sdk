# SPDX-License-Identifier: Apache-2.0
"""Provisioning throughput: --linux-host, SCPI pacing (power.min_gap_s), truthful dsw1 labels."""

from __future__ import annotations

import pytest
from provision import bench, steps
from provision.bench import BenchError

from .provision_fakes import FakeConsole, FakePower, FakeProbe
from .test_provision_bench import _Psu
from .test_provision_steps import _bench, _ctx


# --- 1: --linux-host ---------------------------------------------------------------------

def test_linux_host_flag_reaches_the_attached_host(tmp_path):
    b = _bench()
    ctx = _ctx(tmp_path, bench=b, execute=True)
    b.linux_host = "192.0.2.77"          # what main() does with --linux-host
    assert ctx.need_linux().host == "192.0.2.77"


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


def test_load_bench_reads_power_min_gap_s(tmp_path):
    from .provision_fakes import FakeOperator
    y = tmp_path / "bench.yaml"
    base = """
console: {kind: tcp, host: c, port: 1}
power: {kind: scpi, host: p, port: 5025, channel: 1%s}
probe: null
linux: {user: root}
i2c_bus: {eeprom: 0, pmic: 1, brd: 3}
scif: {flash_writer: w.mot, baud: 115200, program_start: {bl2_mmc: null, fip: 1}}
"""
    y.write_text(base % ", min_gap_s: 0.7", encoding="utf-8")
    assert bench.load_bench(y, FakeOperator()).power.GAP_S == 0.7
    y.write_text(base % "", encoding="utf-8")
    assert bench.load_bench(y, FakeOperator()).power.GAP_S == 0.3
