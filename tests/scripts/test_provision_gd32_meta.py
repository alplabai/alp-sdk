# SPDX-License-Identifier: Apache-2.0
"""gd32_flash erases the whole OTA metadata area and checks what actually runs afterwards.

Bench fact (E1M-V2M103 2026W38-0009): the step wrote the 44-byte record at 0x08008000 and
reported "flashed and verified", but a newer record on another metadata page described the
OLD slot-A image, the bootloader fell back to slot B and the unit ran the old build.
"""

from __future__ import annotations

import binascii

import pytest
from provision import steps
from provision.bench import BenchError, Probe

from .provision_fakes import FakeProbe
from .test_provision_steps import BUILD_ID_REPLY, OTA_STATE_REPLY, Board, _bench, _ctx, _gd32_fw

BUILD_ID = "0.3.1+d53e3d90a19973"
STALE_PAGE = steps.GD32_META_ADDR + 0x800


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    monkeypatch.setattr(steps, "BRIDGE_GAP_S", 0.0)
    monkeypatch.setattr(steps.lt, "I2C_GET_GAP_S", 0.0)


def _setup(tmp_path, probe=None, slot_a=None, **board_kw):
    fw = _gd32_fw(tmp_path)
    if slot_a is not None:
        (fw / "slot-a.bin").write_bytes(slot_a)
    board = Board()
    for k, v in board_kw.items():
        setattr(board, k, v)
    probe = probe or FakeProbe()
    ctx = _ctx(tmp_path, bench=_bench(probe=probe), linux=board, gd32_fw=fw, execute=True)
    return ctx, probe, fw


def _kinds(probe):
    return [c[0] for c in probe.calls]


def test_erase_of_the_whole_area_comes_first_then_the_images_and_the_record(tmp_path):
    ctx, probe, _ = _setup(tmp_path)
    res = steps.Gd32Flash().run(ctx)
    assert _kinds(probe)[:5] == ["dp_id", "erase", "loadbin", "loadbin", "loadbin"]
    assert probe.calls[1] == ("erase", steps.GD32_META_ADDR, 0x2000)
    loads = [c[2] for c in probe.calls if c[0] == "loadbin"]
    assert loads == [steps.GD32_META_ADDR, 0x08000000, 0x0800A000]
    assert res.evidence["gd32_active_slot"] == "A" and res.evidence["gd32_build_id"] == BUILD_ID


def test_ota_meta_md5_keeps_its_shipped_meaning_and_the_area_has_its_own_key(tmp_path):
    import hashlib
    ctx, _, fw = _setup(tmp_path)
    ev = steps.Gd32Flash().run(ctx).evidence
    rec = (fw / "ota-meta.bin").read_bytes()
    assert ev["gd32_ota_meta_md5"] == hashlib.md5(rec).hexdigest()
    assert ev["gd32_ota_meta_area_md5"] == hashlib.md5(rec.ljust(0x2000, bytes((0xFF,)))).hexdigest()


def test_a_planted_newer_record_is_wiped_and_the_probe_is_then_satisfied(tmp_path):
    probe = FakeProbe(memory={STALE_PAGE: b"\x5a" * 44, steps.GD32_META_ADDR: b"\x11" * 44})
    ctx, probe, fw = _setup(tmp_path, probe=probe)
    steps.Gd32Flash().run(ctx)
    out = tmp_path / "area.bin"
    probe.savebin(out, steps.GD32_META_ADDR, steps.GD32_META_SIZE)
    area = out.read_bytes()
    assert area == (fw / "ota-meta.bin").read_bytes().ljust(steps.GD32_META_SIZE, b"\xff")
    assert isinstance(steps.Gd32Flash().probe(ctx), steps.Satisfied)


def test_a_probe_without_erase_is_refused_before_any_write(tmp_path):
    class NoErase(FakeProbe):
        erase = Probe.erase

    ctx, probe, _ = _setup(tmp_path, probe=NoErase())
    with pytest.raises(steps.Refused, match="cannot erase"):
        steps.Gd32Flash().run(ctx)
    assert "loadbin" not in _kinds(probe)


def test_a_stale_record_on_another_metadata_page_is_unsatisfied(tmp_path):
    ctx, _, fw = _setup(tmp_path)
    mem = {a: (fw / n).read_bytes() for n, a, _k in steps.GD32_IMAGES}
    mem[STALE_PAGE] = b"\x5a" * 44                    # a newer A/B record the 44-byte md5 never saw
    ctx.bench.probe.memory = mem
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Unsatisfied) and "stale" in r.reason, r


def test_a_clean_area_is_satisfied(tmp_path):
    ctx, _, fw = _setup(tmp_path)
    ctx.bench.probe.memory = {a: (fw / n).read_bytes() for n, a, _k in steps.GD32_IMAGES}
    r = steps.Gd32Flash().probe(ctx)
    assert isinstance(r, steps.Satisfied), r
    assert r.evidence["gd32_active_slot"] == "A"


def test_slot_b_after_the_flash_fails_the_step_naming_slot_and_build_id(tmp_path):
    ctx, _, _ = _setup(tmp_path, gd32_active=1, gd32_build_id="0.3.1+7da3d3d700398b")
    with pytest.raises(steps.WrongImage, match=r"slot B, build-id '0\.3\.1\+7da3d3d700398b'"):
        steps.Gd32Flash().run(ctx)


def test_probe_reports_slot_b_as_unsatisfied_so_a_rerun_reflashes(tmp_path):
    ctx, _, fw = _setup(tmp_path, gd32_active=1)
    ctx.bench.probe.memory = {a: (fw / n).read_bytes() for n, a, _k in steps.GD32_IMAGES}
    assert isinstance(steps.Gd32Flash().probe(ctx), steps.Unsatisfied)


def test_build_id_embedded_in_slot_a_must_match_the_running_one(tmp_path):
    image = b"\x00" * 16 + BUILD_ID.encode() + b"\x00" * 16
    ctx, _, _ = _setup(tmp_path, slot_a=image)
    assert steps.Gd32Flash().run(ctx).evidence["gd32_build_id"] == BUILD_ID
    (tmp_path / "b").mkdir()
    ctx, _, _ = _setup(tmp_path / "b", slot_a=image, gd32_build_id="0.3.1+7da3d3d700398b")
    with pytest.raises(steps.WrongImage, match="embeds"):
        steps.Gd32Flash().run(ctx)


def test_an_image_without_a_build_id_is_refused_before_any_write(tmp_path):
    ctx, probe, _ = _setup(tmp_path, slot_a=b"" * 32)
    with pytest.raises(steps.Refused, match="embeds no"):
        steps.Gd32Flash().run(ctx)
    assert "erase" not in _kinds(probe) and "loadbin" not in _kinds(probe)


# --- pinned bridge reply vectors and gd32_query -------------------------------------------------

def test_pinned_bridge_vectors_agree_with_an_independent_crc():
    for rsp in (OTA_STATE_REPLY, BUILD_ID_REPLY):
        assert binascii.crc_hqx(rsp[:-2], 0xFFFF) == rsp[-2] | rsp[-1] << 8
    assert steps.lt.gd32_ota_state(Board(), 8) == {"state": 0, "active": 0, "pending": 255,
                                                   "boot_count": 1, "err": 0}
    assert steps.lt.gd32_build_id(Board(), 8) == BUILD_ID


def _status_board(status: int, good_crc: bool = True):
    crc = binascii.crc_hqx(bytes((status,)), 0xFFFF) ^ (0 if good_crc else 0xFFFF)
    out = " ".join(f"0x{b:02x}" for b in (status, crc & 0xFF, crc >> 8, 0, 0, 0, 0, 0, 0)) + "\n"
    b = Board()
    b._answer = lambda cmd: (0, out) if cmd.startswith("i2ctransfer -f -y 8 w4@0x70 0x00 0xf5 ") else b.__class__._answer(b, cmd)
    return b


def test_gd32_query_types_nosupport_and_checks_the_status_crc_first():
    with pytest.raises(steps.lt.Gd32NoSupport):
        steps.lt.gd32_ota_state(_status_board(0x06), 8)
    with pytest.raises(BenchError, match="status 0x01") as e:
        steps.lt.gd32_ota_state(_status_board(0x01), 8)
    assert not isinstance(e.value, steps.lt.Gd32NoSupport)
    with pytest.raises(BenchError, match="bad reply CRC"):
        steps.lt.gd32_ota_state(_status_board(0x06, good_crc=False), 8)


# --- boot_sd_linux's release of the SD hold ------------------------------------------------------

def _hold(monkeypatch, get, clear=None):
    monkeypatch.setattr(steps.lt, "root_device", lambda t: "/dev/mmcblk0p2")
    monkeypatch.setattr(steps.lt, "resolve_emmc", lambda t: "/dev/mmcblk0")
    monkeypatch.setattr(steps.lt, "gd32_boot_config", get)
    monkeypatch.setattr(steps.lt, "gd32_sd_out_flag_set", clear or (lambda t, bus, addr, on: 0))


def test_release_sd_hold_continues_when_the_gd32_does_not_answer(tmp_path, monkeypatch):
    def get(t, bus, f=None, addr=0x70):
        raise BenchError("rc=1: i2ctransfer: no ACK")
    _hold(monkeypatch, get)
    ctx, ev = _ctx(tmp_path, bench=_bench(), linux=Board(), execute=True), {}
    steps.BootSdLinux._release_sd_hold(ctx, ev)
    assert ev["gd32_boot_config_cleared"].startswith("not read (") and "no ACK" in ev["gd32_boot_config_cleared"]


def test_release_sd_hold_still_raises_when_the_clear_fails_after_a_set_flag(tmp_path, monkeypatch):
    def clear(t, bus, addr, on):
        raise BenchError("commit never landed")
    _hold(monkeypatch, lambda t, bus, f=None, addr=0x70: 1, clear)
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(), execute=True)
    with pytest.raises(BenchError, match="commit never landed"):
        steps.BootSdLinux._release_sd_hold(ctx, {})


# --- verification facts do not survive a later change of the unit (2026W38-0009) ----------------
# Driven through run_steps/run_one with small fake steps named like the real ones.

def _fake(name, *, mutate=False, satisfied=False, refuse=False, die=False, evidence=None):
    class Fake(steps.Step):
        trust_run = True

        def probe(self, ctx):
            return steps.Satisfied({}, "already") if satisfied else steps.Unknown("run")

        def run(self, ctx):
            if refuse:
                raise steps.Refused("refused before any write")
            if mutate:
                ctx.mutate("write something", lambda: None)
            if die:
                raise RuntimeError("killed mid-write")
            return self.result(ctx, "ok", dict(evidence or {}))

    Fake.name = name
    return Fake


COLD = {"cold_boots_passed": "3/3"}
FUNC = {"test_functional": "pass"}


def _verify(ctx):
    steps.run_steps(ctx, steps=[_fake("sd_out_emmc_boot"), _fake("cold_boot_test", evidence=COLD),
                                _fake("functional_test", evidence=FUNC)])


def _record_ctx(tmp_path):
    import yaml
    ctx = _ctx(tmp_path, execute=True)
    keys = {
        "eeprom_unique_id": {"group": "identity", "source": "", "mode": "auto", "ship_required": True},
        "cold_boots_passed": {"group": "memory", "source": "", "mode": "auto", "ship_required": True,
                              "step": "cold_boot_test"},
        "test_functional": {"group": "campaign", "source": "", "mode": "auto", "ship_required": False,
                            "step": "functional_test"},
        "disposition": {"group": "disposition", "source": "", "mode": "manual", "ship_required": True},
    }
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": keys}), encoding="utf-8")
    unit = ctx.unit_dir / f"{ctx.serial}.unit.yaml"
    unit.parent.mkdir(parents=True, exist_ok=True)
    unit.write_text("eeprom_unique_id: ab12\ndisposition: ship\ncold_boots_passed: 3/3\n"
                    "test_functional: pass\n", encoding="utf-8")
    return ctx, unit


def _record(ctx, unit):
    res = steps.run_steps(ctx, only=["record"])
    return res[-1].detail, unit.read_text(encoding="utf-8")


def test_verification_then_no_write_is_shippable(tmp_path):
    ctx, unit = _record_ctx(tmp_path)
    _verify(ctx)
    steps.run_steps(ctx, steps=[_fake("write_xspi", satisfied=True)])      # satisfied: no mutation
    assert steps.voided_steps(ctx.state) == {}
    assert "SHIPPABLE" in _record(ctx, unit)[0]


def test_done_then_skipped_write_still_voids_the_earlier_proof(tmp_path):
    """BLOCKER: a write step's LATEST entry was `skipped`, which hid the real write before it."""
    ctx, unit = _record_ctx(tmp_path)
    _verify(ctx)
    steps.run_steps(ctx, steps=[_fake("write_xspi", mutate=True)])            # reflash: done
    assert set(steps.voided_steps(ctx.state)) == {"sd_out_emmc_boot", "cold_boot_test", "functional_test"}
    res = steps.run_steps(ctx, steps=[_fake("write_xspi", satisfied=True)])   # next run: skipped
    assert res[0].status == "skipped" and ctx.state["steps"]["write_xspi"]["status"] == "skipped"
    assert "cold_boot_test" in steps.voided_steps(ctx.state) and not ctx.state_done("cold_boot_test")
    detail, text = _record(ctx, unit)
    assert "SHIPPABLE" not in detail and "verification voided" in detail and "sd_out_emmc_boot" in detail, detail
    assert "cold_boots_passed: unread (voided" in text and "test_functional: unread (voided" in text
    assert "3/3" not in text and "test_functional: pass" not in text      # not resurrected from the entry


def test_the_sd_hold_clear_voids_sd_out_and_cold_boot(tmp_path, monkeypatch):
    """BLOCKER: with every write satisfied, sd_out probed `state file: done` and the flag stayed cleared."""
    ctx, unit = _record_ctx(tmp_path)
    ctx.bench = _bench()
    ctx.linux = Board()
    _verify(ctx)
    assert isinstance(steps.SdOutEmmcBoot().probe(ctx), steps.Satisfied)
    _hold(monkeypatch, lambda t, bus, f=None, addr=0x70: 0)                    # flag not set: nothing to clear
    steps.BootSdLinux._release_sd_hold(ctx, {})
    assert steps.voided_steps(ctx.state) == {}
    _hold(monkeypatch, lambda t, bus, f=None, addr=0x70: 1)                    # flag set: cleared
    ev = {}
    steps.BootSdLinux._release_sd_hold(ctx, ev)
    assert ev["gd32_boot_config_cleared"] == "SDMUX_EN_HIGH"
    assert {"sd_out_emmc_boot", "cold_boot_test"} <= set(steps.voided_steps(ctx.state))
    assert isinstance(steps.SdOutEmmcBoot().probe(ctx), steps.Unknown)         # it runs again
    assert "SHIPPABLE" not in _record(ctx, unit)[0]


def test_a_write_killed_while_running_still_voids(tmp_path):
    ctx, _ = _record_ctx(tmp_path)
    _verify(ctx)
    with pytest.raises(RuntimeError):
        steps.run_steps(ctx, steps=[_fake("write_rootfs", mutate=True, die=True)])
    on_disk = steps.load_state(ctx.state_path)                                # a fresh process sees it
    assert on_disk["steps"]["write_rootfs"]["status"] == "running"
    assert "cold_boot_test" in steps.voided_steps(on_disk)


def test_a_refusal_before_any_mutation_voids_nothing(tmp_path):
    ctx, _ = _record_ctx(tmp_path)
    _verify(ctx)
    res = steps.run_steps(ctx, steps=[_fake("write_cm33", refuse=True)])
    assert res[0].status == "failed" and not ctx.state.get("last_mutation_seq")
    assert steps.voided_steps(ctx.state) == {}
    # the real gd32_flash refusing a probe that cannot erase is such a refusal too
    (tmp_path / "g").mkdir()
    ctx2, _probe, _ = _setup(tmp_path / "g")
    ctx2.bench.probe = type("NoErase", (FakeProbe,), {"erase": Probe.erase})()
    _verify(ctx2)
    assert steps.run_steps(ctx2, only=["gd32_flash"])[-1].status == "failed"
    assert not ctx2.state.get("last_mutation_seq") and steps.voided_steps(ctx2.state) == {}


def test_proof_in_a_superseded_group_is_voided_by_a_write_in_the_new_one(tmp_path, monkeypatch):
    ctx, unit = _record_ctx(tmp_path)
    monkeypatch.setattr(steps, "tool_rev", lambda: "rev-a")
    steps.init_state(ctx, "bundle")
    _verify(ctx)
    monkeypatch.setattr(steps, "tool_rev", lambda: "rev-b")
    steps.init_state(ctx, "bundle")                       # same bundle, new tool: proof moves to `superseded`
    assert len(ctx.state["superseded"]) == 1 and steps.voided_steps(ctx.state) == {}
    steps.run_steps(ctx, steps=[_fake("write_xspi", mutate=True)])
    assert {"cold_boot_test", "functional_test"} <= set(steps.voided_steps(ctx.state))
    detail, text = _record(ctx, unit)
    assert "SHIPPABLE" not in detail and "cold_boots_passed: unread (voided" in text


def test_a_verification_after_the_write_stands(tmp_path):
    ctx, unit = _record_ctx(tmp_path)
    steps.run_steps(ctx, steps=[_fake("gd32_flash", mutate=True)])
    _verify(ctx)
    assert steps.voided_steps(ctx.state) == {} and ctx.state_done("cold_boot_test")
    assert "SHIPPABLE" in _record(ctx, unit)[0]


def _old_state(**entries):
    """A state file from before `seq`: entries carry only `at`, there is no marker."""
    return {"steps": {n: {"status": st, "at": at, "evidence": ev}
                      for n, (st, at, ev) in entries.items()}}


def test_old_state_with_a_write_after_the_proof_is_voided_once_and_saved(tmp_path):
    ctx, unit = _record_ctx(tmp_path)
    ctx.state = _old_state(cold_boot_test=("done", "2026-10-21T12:46:41Z", COLD),
                           write_xspi=("done", "2026-10-21T13:30:00Z", {}))
    assert "cold_boot_test" in steps.voided_steps(ctx.state)             # the read-only view
    assert "last_mutation_seq" not in ctx.state
    steps.run_steps(ctx, steps=[_fake("census")])                         # run_steps seeds and saves
    on_disk = steps.load_state(ctx.state_path)
    assert on_disk["last_mutation_seq"] >= 1 and "cold_boot_test" in steps.voided_steps(on_disk)
    marker = on_disk["last_mutation_seq"]
    steps.run_steps(ctx, steps=[_fake("write_xspi", satisfied=True)])     # never derived again
    assert steps.load_state(ctx.state_path)["last_mutation_seq"] == marker
    detail, _ = _record(ctx, unit)
    assert "SHIPPABLE" not in detail and "cold_boot_test" in detail


def test_old_state_without_a_write_after_the_proof_stands(tmp_path):
    ctx, unit = _record_ctx(tmp_path)
    ctx.state = _old_state(write_xspi=("done", "2026-10-21T12:00:00Z", {}),
                           cold_boot_test=("done", "2026-10-21T12:46:41Z", COLD),
                           functional_test=("done", "2026-10-21T12:50:00Z", FUNC))
    assert steps.voided_steps(ctx.state) == {}
    steps.run_steps(ctx, steps=[_fake("census")])
    assert steps.load_state(ctx.state_path)["last_mutation_seq"] >= 1     # the old write, before the proof
    assert steps.voided_steps(ctx.state) == {} and ctx.state_done("cold_boot_test")
    assert "SHIPPABLE" in _record(ctx, unit)[0]
    steps.run_steps(ctx, steps=[_fake("write_xspi", mutate=True)])        # a NEW mutation still voids it
    assert "cold_boot_test" in steps.voided_steps(ctx.state)


def test_old_state_across_superseded_groups(tmp_path):
    ctx, _ = _record_ctx(tmp_path)
    ctx.state = {**_old_state(write_xspi=("skipped", "2026-10-21T14:00:00Z", {})), "superseded": [
        _old_state(cold_boot_test=("done", "2026-10-21T12:46:41Z", COLD),
                   write_rootfs=("done", "2026-10-21T12:00:00Z", {}))]}
    assert steps.voided_steps(ctx.state) == {}                            # the write is older, the later skip is no write
    ctx.state["superseded"][0]["steps"]["write_rootfs"]["at"] = "2026-10-21T13:00:00Z"
    assert "cold_boot_test" in steps.voided_steps(ctx.state)


def test_a_write_killed_mid_step_does_not_keep_its_old_done_evidence(tmp_path):
    ctx, _ = _record_ctx(tmp_path)
    steps.run_steps(ctx, steps=[_fake("write_rootfs", mutate=True, evidence={"rootfs_wic_sha256": "old"})])
    assert ctx.state["steps"]["write_rootfs"]["evidence"] == {"rootfs_wic_sha256": "old"}
    with pytest.raises(RuntimeError):
        steps.run_steps(ctx, steps=[_fake("write_rootfs", mutate=True, die=True)])
    killed = steps.load_state(ctx.state_path)["steps"]["write_rootfs"]
    assert killed["status"] == "running" and "evidence" not in killed


def test_mixed_seq_without_a_marker_is_renumbered_by_wall_clock(tmp_path):
    """The shape of the real 2026W38-0009 state: an intermediate tool numbered only the newest
    entries (5, 6), older writes in the superseded groups and this group carry none, no marker.
    Every write is older than the proofs, so nothing is voided."""
    def e(status, at, seq=None):
        return {"status": status, "at": at, "evidence": {}, **({} if seq is None else {"seq": seq})}
    ctx, _ = _record_ctx(tmp_path)
    ctx.state = {"seq": 7, "bundle_sha256": "b", "tool_rev": "c", "steps": {
        "gd32_flash": e("done", "2026-10-21T20:11:23Z"), "write_xspi": e("skipped", "2026-10-21T20:11:25Z"),
        "cold_boot_test": e("done", "2026-10-21T20:15:48Z"),
        "census_final": e("done", "2026-10-21T20:48:40Z", 5),
        "functional_test": e("done", "2026-10-21T20:48:48Z", 6),
        "record": e("done", "2026-10-21T20:48:50Z", 7)}, "superseded": [
        {"tool_rev": "a", "bundle_sha256": "b", "steps": {
            "gd32_flash": e("done", "2026-10-21T09:42:14Z"), "write_rootfs": e("done", "2026-10-21T09:44:30Z"),
            "cold_boot_test": e("done", "2026-10-21T12:46:41Z"),
            "functional_test": e("done", "2026-10-21T13:30:24Z")}}]}
    assert steps.voided_steps(ctx.state) == {}                            # the read-only view
    steps.run_steps(ctx, steps=[_fake("census")])
    on_disk = steps.load_state(ctx.state_path)
    assert steps.voided_steps(on_disk) == {} and ctx.state_done("cold_boot_test")
    assert on_disk["steps"]["gd32_flash"]["seq"] < on_disk["steps"]["cold_boot_test"]["seq"]
    assert on_disk["superseded"][0]["steps"]["write_rootfs"]["seq"] < on_disk["steps"]["gd32_flash"]["seq"]
    steps.run_steps(ctx, steps=[_fake("write_xspi", mutate=True)])        # a real mutation after it voids
    assert {"cold_boot_test", "census_final", "functional_test"} <= set(steps.voided_steps(ctx.state))
