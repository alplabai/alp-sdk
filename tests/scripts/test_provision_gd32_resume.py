# SPDX-License-Identifier: Apache-2.0
"""gd32_flash never leaves the GD32 halted over SWD, and census_final re-reads the bus (#2624).

A `savebin` dump halts the core and leaves it halted. A halted GD32 still ACKs its I2C
address and stretches SCL, so the board-management bus wedges until the next power cycle.
The fakes model exactly that: ``HaltingProbe.halted`` is set by every savebin / loadbin and
cleared only by reset_run, and ``WedgeBoard`` fails every bus-8 transfer while it is set.
"""

from __future__ import annotations

import pytest
import yaml
from provision import steps
from provision.bench import BenchError

from .provision_fakes import FakeProbe
from .test_provision_steps import CATALOGUE, SERIAL, Board, _bench, _ctx, _gd32_fw, _statuses


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    monkeypatch.setattr(steps, "BRIDGE_GAP_S", 0.0)
    monkeypatch.setattr(steps.lt, "I2C_GET_GAP_S", 0.0)


class HaltingProbe(FakeProbe):
    """FakeProbe with the real tools' side effect: a dump or a write halts the core."""

    def __init__(self, *a, fail: str = "", resume_ok: int = 99, corrupt: bool = False, **kw):
        """``resume_ok``: how many reset_run calls work; later ones leave the core halted
        (or raise, with ``fail="reset_run"``)."""
        super().__init__(*a, **kw)
        self.halted, self.fail, self.resume_ok, self.corrupt = False, fail, resume_ok, corrupt

    def savebin(self, path, addr, size):
        self.halted = True
        if self.fail == "savebin":
            self.calls.append(("savebin", path, addr, size))
            raise BenchError("fake: dump died mid-read")
        super().savebin(path, addr, size)
        if self.corrupt:
            path.write_bytes(b"\x00" * size)

    def loadbin(self, path, addr):
        self.halted = True
        if self.fail == "loadbin":
            self.calls.append(("loadbin", path, addr))
            raise BenchError("fake: FMC busy timeout")
        super().loadbin(path, addr)

    def reset_run(self):
        super().reset_run()
        self.resume_ok -= 1
        if self.resume_ok >= 0:
            self.halted = False
        elif self.fail == "reset_run":
            raise BenchError("fake: no ack")


class WedgeBoard(Board):
    """Board whose i2c-8 is dead while the GD32 is halted (or while ``wedged`` is forced)."""

    def __init__(self, probe=None, wedged=False, **kw):
        super().__init__(**kw)
        self.probe, self.wedged = probe, wedged

    def _dead(self):
        return self.wedged or (self.probe is not None and self.probe.halted)

    def _answer(self, cmd):
        if self._dead() and (cmd.startswith(("i2cget -y -f 8 ", "i2ctransfer -f -y 8 ", "i2cdetect -y -r 8"))):
            return 2, ""
        if cmd.startswith("dmesg | grep -c 'SCL is stuck low'"):
            return (0, "115\n") if self._dead() else (1, "0\n")
        return super()._answer(cmd)


def _setup(tmp_path, flashed=True, **probe_kw):
    fw = _gd32_fw(tmp_path)
    mem = {a: (fw / n).read_bytes() for n, a, _k in steps.GD32_IMAGES} if flashed else {}
    probe = HaltingProbe(memory=mem, **probe_kw)
    board = WedgeBoard(probe=probe)
    ctx = _ctx(tmp_path, bench=_bench(probe=probe), linux=board, gd32_fw=fw, execute=True)
    return ctx, probe, board


def _kinds(probe):
    return [c[0] for c in probe.calls]


def _every_dump_is_resumed(probe):
    """After the LAST savebin of every run of savebins there is a reset_run before anything else."""
    kinds = _kinds(probe)
    for i, k in enumerate(kinds):
        if k == "savebin" and (i + 1 == len(kinds) or kinds[i + 1] != "savebin"):
            if i + 1 == len(kinds) or kinds[i + 1] != "reset_run":
                return False
    return True


# --- every halting call site ------------------------------------------------------------

def test_satisfied_probe_resumes_the_core_and_confirms_the_bridge(tmp_path):
    ctx, probe, _ = _setup(tmp_path)
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Satisfied), r
    assert _kinds(probe) == ["dp_id", "savebin", "savebin", "savebin", "reset_run"]
    assert probe.halted is False
    assert r.evidence["gd32_bridge_after_readback"] == "GD32 bridge protocol 0.13.0"


def test_unsatisfied_probe_still_resumes_the_core(tmp_path):
    ctx, probe, _ = _setup(tmp_path, flashed=False)
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Unsatisfied), r
    assert _kinds(probe)[-1] == "reset_run" and probe.halted is False


def test_probe_resumes_the_core_when_the_dump_raises(tmp_path):
    ctx, probe, _ = _setup(tmp_path, fail="savebin")
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Unknown) and "dump died" in r.reason
    assert _kinds(probe) == ["dp_id", "savebin", "reset_run"]
    assert probe.halted is False


def test_run_resumes_after_write_and_verify_and_the_post_run_probe(tmp_path):
    ctx, probe, board = _setup(tmp_path, flashed=False)
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    st = _statuses(res)
    assert st["gd32_flash"] == "done", res[1].detail
    assert _every_dump_is_resumed(probe), _kinds(probe)
    # pre-run probe, run (3 writes + 3 verify dumps), post-run probe: one resume each
    assert _kinds(probe).count("reset_run") == 3
    assert probe.halted is False
    assert ctx.facts["gd32_protocol"] == "GD32 bridge protocol 0.13.0"
    assert ctx.facts["gd32_bridge_after_readback"] == "GD32 bridge protocol 0.13.0"
    # the census that follows reads bus 8
    census = next(r for r in res if r.name == "census")
    assert census.evidence["act88760_gpio_regs"] == "0x10=0x08"
    assert "wedged" not in census.detail


@pytest.mark.parametrize("fail", ["loadbin", "savebin"])
def test_run_resumes_the_core_when_the_write_or_the_verify_dump_raises(tmp_path, fail):
    ctx, probe, _ = _setup(tmp_path, flashed=False)
    step = steps.Gd32Flash()
    probe.fail = fail
    with pytest.raises(BenchError):
        step.run(ctx)
    assert _kinds(probe)[-1] == "reset_run" and probe.halted is False


def test_run_resumes_the_core_when_the_verify_md5_differs(tmp_path):
    ctx, probe, _ = _setup(tmp_path, flashed=False, corrupt=True)
    with pytest.raises(BenchError, match="does not match"):
        steps.Gd32Flash().run(ctx)
    assert _kinds(probe)[-1] == "reset_run" and probe.halted is False


# --- the bridge must be back before the step returns ---------------------------------------

def test_step_fails_when_the_bridge_does_not_come_back(tmp_path):
    ctx, probe, _ = _setup(tmp_path, flashed=False, resume_ok=1)      # the reset after the flash does nothing
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    st = _statuses(res)
    assert st["gd32_flash"] == "failed"
    assert "did not answer GET_VERSION" in res[1].detail and "SCL is stuck low" in res[1].detail
    assert "census" not in st                      # never continues into census on a wedged bus


def test_step_names_the_wedge_when_the_bus_is_already_dead_before_the_flash(tmp_path):
    ctx, probe, _ = _setup(tmp_path, flashed=False, resume_ok=0)      # the pre-run probe's reset does nothing
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    st = _statuses(res)
    assert st["gd32_flash"] == "failed" and "census" not in st
    assert "SCL is stuck low" in res[1].detail and "power-cycle" in res[1].detail
    assert "loadbin" not in _kinds(probe)          # nothing is written through a wedged bus


def test_already_flashed_unit_is_not_skipped_when_the_bridge_stays_dead(tmp_path):
    ctx, probe, _ = _setup(tmp_path, resume_ok=0)
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Unknown) and "firmware matches, but" in r.reason
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    assert _statuses(res)["gd32_flash"] == "failed" and "census" not in _statuses(res)


def test_step_fails_when_the_reset_itself_fails(tmp_path):
    ctx, probe, _ = _setup(tmp_path, flashed=False, fail="reset_run", resume_ok=1)
    res = steps.run_steps(ctx, only=["gd32_flash"])
    assert _statuses(res)["gd32_flash"] == "failed"
    assert "core left halted" in res[1].detail
    assert any("reset-and-run FAILED" in ln for ln in ctx.plan_log)


def test_a_failed_reset_in_the_pre_run_probe_is_the_step_result(tmp_path):
    """The probe's readback halts the core and its reset fails: the step result must say "core
    left halted" (not a bare I2C read error), and nothing is written."""
    ctx, probe, _ = _setup(tmp_path, flashed=False, fail="reset_run", resume_ok=0)
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, (steps.Unknown, steps.Unsatisfied))
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    st = _statuses(res)
    assert st["gd32_flash"] == "failed" and "census" not in st
    assert "core left halted by the pre-run probe's readback" in res[1].detail and "fake: no ack" in res[1].detail
    assert "loadbin" not in _kinds(probe)


def test_a_failed_reset_never_replaces_the_original_error(tmp_path):
    # the verify md5 differs AND the reset then fails: both are in the reason, the verify error first
    ctx, probe, _ = _setup(tmp_path, flashed=False, corrupt=True, fail="reset_run", resume_ok=1)
    res = steps.run_steps(ctx, only=["gd32_flash"])
    d = res[1].detail
    assert "does not match" in d and "ALSO the reset-and-run failed, GD32 core left halted: fake: no ack" in d
    assert d.index("does not match") < d.index("ALSO")
    # the dump itself dies in the probe AND the reset fails: the probe's reason carries both
    (tmp_path / "b").mkdir()
    ctx, probe, _ = _setup(tmp_path / "b", resume_ok=0)
    probe.fail = "savebin"

    def boom():
        probe.calls.append(("reset_run",))
        raise RuntimeError("probe wrapper crashed")           # not even a BenchError
    probe.reset_run = boom
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Unknown)
    assert "dump died" in r.reason and "core left halted: RuntimeError: probe wrapper crashed" in r.reason


def test_a_later_good_reset_clears_the_halted_marker(tmp_path):
    ctx, probe, _ = _setup(tmp_path, fail="reset_run", resume_ok=0)
    steps.Gd32Flash().probe(ctx)
    assert ctx._cache.get("gd32_halted")
    probe.resume_ok = 99
    assert steps.Gd32Flash._resume(ctx, probe) == "" and "gd32_halted" not in ctx._cache


# --- mutation: without the resume the modelled bus wedges and the tests above fail -----------

def test_mutation_without_the_resume_the_bus_wedges_and_census_reads_nothing(tmp_path, monkeypatch):
    monkeypatch.setattr(steps.Gd32Flash, "_resume", staticmethod(lambda ctx, probe: ""))
    ctx, probe, _ = _setup(tmp_path)
    r = steps.Gd32Flash().probe(ctx)
    assert probe.halted is True                    # this is the defect: the dump left the core halted
    assert not _every_dump_is_resumed(probe)
    assert isinstance(r, steps.Unknown)            # and the bridge check now catches it
    res = steps.run_steps(ctx, only=["census"])
    census = next(r for r in res if r.name == "census")
    assert census.evidence["act88760_gpio_regs"].startswith("unread (")
    assert "I2C bus wedged: 115 'SCL is stuck low'" in census.detail


# --- census_final: the unit as shipped ---------------------------------------------------

def test_census_final_runs_after_the_last_cold_boot_and_before_record():
    n = steps.STEP_NAMES
    assert n.index("cold_boot_test") < n.index("census_final") < n.index("record")
    assert n.index("census") < n.index("cold_boot_test")


def _ledger_with(tmp_path, *keys):
    cat = {**CATALOGUE, "keys": {**CATALOGUE["keys"], **{
        k: {"group": "power", "source": "", "mode": "auto", "ship_required": True} for k in keys}}}
    root = tmp_path / "ledger-ext"          # _ctx always builds its default tmp_path/ledger too
    (root / "schema").mkdir(parents=True)
    (root / "schema" / "v2n.keys.yaml").write_text(yaml.safe_dump(cat), encoding="utf-8")
    return root


def test_census_final_replaces_the_unread_values_of_a_wedged_first_census(tmp_path):
    board = WedgeBoard(wedged=True)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True,
               ledger_root=_ledger_with(tmp_path, "act88760_gpio_regs"))
    res = steps.run_steps(ctx, only=["census", "record"])
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    assert "act88760_gpio_regs: unread (" in unit.read_text(encoding="utf-8")
    assert "missing act88760_gpio_regs" in res[-1].detail          # the unread value blocks shipping
    board.wedged = False                                            # the power cycle cleared the wedge
    res = steps.run_steps(ctx, only=["census_final", "record"])
    assert "act88760_gpio_regs: 0x10=0x08" in unit.read_text(encoding="utf-8")
    assert "missing act88760_gpio_regs" not in res[-1].detail


def test_census_final_still_unread_keeps_the_unit_blocked(tmp_path):
    board = WedgeBoard(wedged=True)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True,
               ledger_root=_ledger_with(tmp_path, "act88760_gpio_regs"))
    res = steps.run_steps(ctx, only=["census_final", "record"])
    assert "missing act88760_gpio_regs" in res[-1].detail


def test_census_final_leaves_the_gpio4_verdict_to_cold_boot_test(tmp_path):
    """After a plain cold boot reg 0x10 reads released whoever released it: census_final must
    not overwrite cold_boot_test's `0x88` / `u-boot` with `0x08` / `none`."""
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(act_0x10=0x08), execute=True)
    ctx.facts.update(act88760_gpio4_otp="0x88", act88760_gpio4_workaround="u-boot")
    res = steps.run_steps(ctx, only=["census_final"])
    final = next(r for r in res if r.name == "census_final")
    assert "act88760_gpio4_otp" not in final.evidence and "act88760_gpio4_workaround" not in final.evidence
    assert ctx.facts["act88760_gpio4_otp"] == "0x88" and ctx.facts["act88760_gpio4_workaround"] == "u-boot"
