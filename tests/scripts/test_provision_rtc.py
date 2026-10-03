# SPDX-License-Identifier: Apache-2.0
"""rtc_set step + provision/rtc.py: clock set, backup switchover through the driver. No hardware."""
from __future__ import annotations

import time

import pytest
from provision import linux_target as lt
from provision import rtc, steps

from .test_provision_steps import Board, _bench, _ctx


class RtcBoard(Board):
    """Reg 0x37 of the RV-3028 plus the commands the step issues."""

    def __init__(self, reg=0x30, epoch="now", **kw):
        super().__init__(**kw)
        self.regs[(8, 0x52, 0x37)] = reg
        self.epoch, self.sets = epoch, []

    def _answer(self, cmd):
        if cmd.startswith("date -u -s @"):
            self.sets.append(cmd)
            return 0, ""
        if cmd.startswith("hwclock"):
            self.sets.append(cmd)
            return 0, ""
        if cmd.startswith("python3 -c") and "0x40187014" in cmd:
            self.sets.append("bsm")
            self.regs[(8, 0x52, 0x37)] |= 0x0C          # BSM = 0b11
            return 0, ""
        if cmd == "cat /sys/class/rtc/rtc0/since_epoch":
            return (0, f"{int(time.time()) if self.epoch == 'now' else self.epoch}\n") if self.epoch != "" else (1, "")
        return super()._answer(cmd)


@pytest.mark.parametrize("reg, mode, trickle", [
    (0x10, "disabled", "disabled"),          # as read on the bench: FEDE only
    (0x2C, "level", "3 kOhm"),               # TCE, BSM 11, TCR 00
    (0x3F, "level", "15 kOhm"),              # TCE, FEDE, BSM 11, TCR 11
    (0x25, "direct", "5 kOhm"),              # TCE, BSM 01, TCR 01
    (0x08, "disabled", "disabled"),          # BSM 10 is disabled too
])
def test_decode_register_0x37(reg, mode, trickle):
    d = rtc.decode(reg)
    assert (d["rtc_backup_switch_mode"], d["rtc_trickle"]) == (mode, trickle)
    assert d["rtc_rv3028_reg_0x37"] == f"{reg:#04x}"


def test_enable_backup_writes_only_when_switchover_is_off():
    b = RtcBoard(reg=0x30)
    assert rtc.enable_backup(b, 8, 0x52) == 0x3C and b.sets == ["bsm"]
    assert rtc.enable_backup(b, 8, 0x52) == 0x3C and b.sets == ["bsm"]      # already level: no second EEPROM commit


def test_rtc_set_sets_clock_enables_switchover_and_reads_back(tmp_path):
    b = RtcBoard(reg=0x33)                      # TCE + FEDE + TCR 15 k, switchover off
    ctx = _ctx(tmp_path, bench=_bench(), linux=b, execute=True)
    r = steps.run_steps(ctx, only=["rtc_set"])[-1]
    assert r.status == "done", r.detail
    assert b.sets[0].startswith("date -u -s @") and b.sets[1].startswith("hwclock") and b.sets[2] == "bsm"
    assert r.evidence["rtc_backup_switch_mode"] == "level" and r.evidence["rtc_trickle"] == "15 kOhm"
    assert r.evidence["rtc_time_utc"].endswith("Z")


def test_rtc_set_keeps_the_time_the_retention_fixture_set(tmp_path):
    b = RtcBoard(reg=0x3C)
    ctx = _ctx(tmp_path, bench=_bench(), linux=b, execute=True)
    ctx.facts["rtc_set_boot_id"] = "boot-1"
    r = steps.run_steps(ctx, only=["rtc_set"])[-1]
    assert r.status == "done" and b.sets == []


def test_rtc_set_fails_when_the_clock_reads_back_unset_or_wrong(tmp_path):
    for n, epoch in enumerate(("", str(int(time.time()) - 3600))):
        ctx = _ctx(tmp_path / str(n), bench=_bench(), linux=RtcBoard(reg=0x3F, epoch=epoch), execute=True)
        assert steps.run_steps(ctx, only=["rtc_set"])[-1].status == "failed"


def test_rtc_set_dry_run_touches_nothing(tmp_path):
    b = RtcBoard(reg=0x10)
    ctx = _ctx(tmp_path, bench=_bench(), linux=b, execute=False)
    assert steps.run_steps(ctx, only=["rtc_set"])[-1].status == "planned"
    assert not any(c.startswith(("date", "hwclock", "python3")) for c in b.commands)


def test_hwclock_missing_falls_back_to_the_rtc0_ioctl():
    class NoHwclock(RtcBoard):
        def _answer(self, cmd):
            if cmd.startswith("hwclock"):
                return 127, ""
            if cmd == "cat /proc/sys/kernel/random/boot_id":
                return 0, "boot-0\n"
            if cmd.startswith("python3 -c") and "0x4024700a" in cmd:
                self.sets.append("ioctl")
                return 0, ""
            return super()._answer(cmd)
    b = NoHwclock()
    rtc.set_time(b, int(time.time()))
    assert b.sets[-1] == "ioctl"


def test_enable_backup_raises_if_the_driver_did_not_take_it():
    class Stuck(RtcBoard):
        def _answer(self, cmd):
            if cmd.startswith("python3 -c"):
                return 0, ""
            return super()._answer(cmd)
    with pytest.raises(lt.BenchError, match="not enabled"):
        rtc.enable_backup(Stuck(reg=0x30), 8, 0x52)
