# SPDX-License-Identifier: Apache-2.0
"""Per-bundle clkgen expectation, sd_out_emmc_boot on a GD32 without BOOT_CONFIG, and
census_final's read-only secure-page re-verification."""

from __future__ import annotations

import pytest
from provision import steps
from provision import linux_target as lt
from provision.bench import BenchError

from .provision_fakes import FakeOperator
from .test_provision_steps import SERIAL, SKU, Board, _bench, _ctx

SOM_022 = {0x1F: 0x46, 0x21: 0xC0, 0x24: 0x8E}   # som-0.2.2: U-Boot patch 0007 only
BUNDLE = {"sku": SKU, "release_version": "som-9.9.9"}   # what test_provision_steps._bundle builds


def _overlay(fixup=SOM_022, bundle=BUNDLE):
    doc = {"schema": 1, "clkgen_fixup_regs": {f"{r:#04x}": f"{v:#04x}" for r, v in fixup.items()}}
    if bundle is not None:
        doc["bundle"] = bundle
    return doc


def _clkgen_board(fixup):
    board = Board()
    image = bytearray(lt.CLKGEN_OTP_IMAGE)
    for reg, val in fixup.items():
        image[reg] = val
    for reg, val in enumerate(image):
        board.regs[(8, 0x69, reg)] = val
    return board


def _clkgen(tmp_path, board, expect):
    tmp_path.mkdir(parents=True, exist_ok=True)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True, functest_expect=expect)
    ctx.boot_text = "NOTICE:  BL2: v2.10\nALP: 5L35023B clock: success\n\ne1m login: "
    return steps.run_steps(ctx, only=["clkgen_verify"])[-1]


# --- clkgen_fixup_regs ---------------------------------------------------------------------

def test_clkgen_overlay_pins_the_bundles_own_fixup(tmp_path):
    r = _clkgen(tmp_path, _clkgen_board(SOM_022), _overlay())
    assert r.status == "done", r.detail
    src = r.evidence["clkgen_fixup_source"]
    assert "functest-expect overlay clkgen_fixup_regs" in src and "0x1f: 0x46" in src
    assert f"bundle {SKU} som-9.9.9" in src


def test_clkgen_without_the_key_keeps_the_tools_default(tmp_path):
    r = _clkgen(tmp_path, _clkgen_board(SOM_022), {"schema": 1})     # som-0.2.2 board, default judge
    assert r.status == "failed" and "reg 0x1f: 0x46 != 0xc7" in r.detail
    r = _clkgen(tmp_path / "b", _clkgen_board(lt.CLKGEN_FIXUP_REGS), {"schema": 1})
    assert r.status == "done" and r.evidence["clkgen_fixup_source"].startswith("default")


def test_clkgen_overlay_still_catches_a_wrong_value(tmp_path):
    r = _clkgen(tmp_path, _clkgen_board({**SOM_022, 0x24: 0x8F}), _overlay())
    assert r.status == "failed" and "reg 0x24: 0x8f != 0x8e" in r.detail


@pytest.mark.parametrize("bundle", [
    None, {}, {"sku": SKU}, {"sku": SKU, "release_version": "som-0.2.2"},
    {"sku": "E1M-OTHER", "release_version": "som-9.9.9"}, "som-9.9.9",
])
def test_clkgen_overlay_for_another_bundle_or_none_refuses(tmp_path, bundle):
    r = _clkgen(tmp_path, _clkgen_board(SOM_022), _overlay(bundle=bundle))
    assert r.status == "failed" and "clkgen_fixup_regs" in r.detail
    assert "bundle" in r.detail


@pytest.mark.parametrize("bad", [
    [], {}, "0x1f", {"0x1f": "0x100"}, {"0x1f": "-1"}, {"0x25": "0x01"}, {"zz": "1"}, {"0x1f": "x"},
    {"0x1f": 0x46}, {0x1F: "0x46"}, {"0x1f": True}, {"0x1f": 1.5}, {"0x1f": "010"}, {"010": "0x01"},
    {"0x1f": "0x01", "0X1F": "0x02"},
])
def test_clkgen_fixup_malformed_is_rejected(bad):
    with pytest.raises(ValueError, match="clkgen_fixup_regs"):
        lt.clkgen_fixup_from_expect({"clkgen_fixup_regs": bad, "bundle": BUNDLE}, SKU, "som-9.9.9")


def test_clkgen_fixup_accepts_quoted_hex_and_absence():
    ok = {"clkgen_fixup_regs": {"0x1F": "0x46"}, "bundle": BUNDLE}
    assert lt.clkgen_fixup_from_expect(ok, SKU, "som-9.9.9") == {0x1F: 0x46}
    assert lt.clkgen_fixup_from_expect({}, SKU, "x") is None and lt.clkgen_fixup_from_expect(None) is None


def test_clkgen_malformed_overlay_refuses_the_step(tmp_path):
    r = _clkgen(tmp_path, _clkgen_board(SOM_022), {**_overlay(), "clkgen_fixup_regs": {"0x1f": "0x999"}})
    assert r.status == "failed" and "clkgen_fixup_regs" in r.detail


# --- sd_out_emmc_boot on a GD32 without BOOT_CONFIG ----------------------------------------

EMMC = "/dev/mmcblk0"


def _sd_out(tmp_path, monkeypatch, roots, cards, proto=(0, 14, 0), operator=None, emmc=EMMC, **kw):
    log: list[str] = []
    it = iter(roots)

    def no_support(t, bus, addr):
        raise lt.Gd32NoSupport("GD32 BOOT_CONFIG status 0x06 (NOSUPPORT)")
    monkeypatch.setattr(steps.lt, "root_device", lambda t: next(it))
    monkeypatch.setattr(steps.lt, "resolve_emmc", lambda t: emmc)
    monkeypatch.setattr(steps.lt, "gd32_sd_out_flag_set", no_support)
    monkeypatch.setattr(steps.lt, "gd32_bridge_version", lambda t, bus, addr: proto)
    monkeypatch.setattr(steps.lt, "mmc_devices", lambda t: cards)
    monkeypatch.setattr(steps.lt, "sdio_mux_set", lambda t, c: log.append("mux"))
    monkeypatch.setattr(steps, "clean_shutdown", lambda ctx: log.append("halt") or "halted")
    monkeypatch.setattr(steps, "cold_boot_phy_retry", lambda ctx, ev: log.append("cold") or "")
    op = operator or FakeOperator()
    ctx = _ctx(tmp_path, bench=_bench(operator=op), linux=Board(), execute=True, **kw)
    return ctx, log, op


def test_sd_out_nosupport_prompts_the_operator_then_verifies_no_sd(tmp_path, monkeypatch):
    ctx, log, op = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk1p2", "/dev/mmcblk0p2"], {"mmc0:0001": "MMC"})
    res = steps.SdOutEmmcBoot().run(ctx)
    assert res.status == "done"
    assert any("REMOVE the microSD" in m for m in op.messages)
    assert log == ["halt", "cold"]                           # no mux write: the card is physically gone
    assert res.evidence["sd_out_method"] == "operator-removed"
    assert "mmc0:0001 MMC" in res.evidence["sd_out_evidence"] and "/dev/mmcblk0p2" in res.evidence["sd_out_evidence"]


def test_sd_out_operator_abort_is_not_done(tmp_path, monkeypatch):
    ctx, log, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk1p2"], {}, operator=FakeOperator(["abort"]))
    with pytest.raises(BenchError, match="aborted"):
        steps.SdOutEmmcBoot().run(ctx)
    assert "cold" not in log


def test_sd_out_noninteractive_without_the_flag_refuses(tmp_path, monkeypatch):
    ctx, log, op = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk1p2"], {})

    def eof(prompt):
        raise EOFError
    op._ask = eof
    with pytest.raises(steps.Refused, match="--sd-removed-by-operator"):
        steps.SdOutEmmcBoot().run(ctx)
    assert "cold" not in log


def test_sd_out_flag_path_halts_cold_cycles_and_verifies_the_fresh_boot(tmp_path, monkeypatch):
    ctx, log, op = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk0p2", "/dev/mmcblk0p2"],
                           {"mmc0:0001": "MMC"}, sd_removed_by_operator=True)
    res = steps.SdOutEmmcBoot().run(ctx)
    assert res.status == "done" and res.evidence["sd_out_method"] == "operator-removed"
    assert op.messages == [] and log == ["halt", "cold"]


def test_sd_out_emmc_root_without_the_flag_refuses(tmp_path, monkeypatch):
    ctx, log, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk0p2"], {"mmc0:0001": "MMC"})
    with pytest.raises(steps.Refused, match="--sd-removed-by-operator"):
        steps.SdOutEmmcBoot().run(ctx)
    assert "cold" not in log


def test_sd_out_flag_but_sd_is_the_root_refuses(tmp_path, monkeypatch):
    ctx, log, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk1p2"], {}, sd_removed_by_operator=True)
    with pytest.raises(steps.Refused, match="cannot have been removed"):
        steps.SdOutEmmcBoot().run(ctx)
    assert "cold" not in log


def test_sd_out_card_still_enumerated_after_the_removal_refuses(tmp_path, monkeypatch):
    for typ in ("SD", "SDcombo"):
        ctx, _, _ = _sd_out(tmp_path / typ, monkeypatch, ["/dev/mmcblk0p2", "/dev/mmcblk0p2"],
                            {"mmc0:0001": "MMC", "mmc1:0001": typ}, sd_removed_by_operator=True)
        with pytest.raises(steps.Refused, match="still present"):
            steps.SdOutEmmcBoot().run(ctx)


def test_sd_out_an_sdio_device_is_not_the_microsd(tmp_path, monkeypatch):
    ctx, _, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk0p2", "/dev/mmcblk0p2"],
                        {"mmc0:0001": "MMC", "mmc2:0001": "SDIO"}, sd_removed_by_operator=True)
    assert steps.SdOutEmmcBoot().run(ctx).status == "done"


def test_sd_out_no_mmc_enumerated_refuses(tmp_path, monkeypatch):
    ctx, _, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk0p2", "/dev/mmcblk0p2"], {},
                        sd_removed_by_operator=True)
    with pytest.raises(steps.Refused, match="no eMMC enumerated"):
        steps.SdOutEmmcBoot().run(ctx)


def test_sd_out_root_neither_sd_nor_emmc_after_the_boot_refuses(tmp_path, monkeypatch):
    ctx, _, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk0p2", "/dev/nvme0n1p2"],
                        {"mmc0:0001": "MMC"}, sd_removed_by_operator=True)
    with pytest.raises(steps.Refused, match="not the eMMC"):
        steps.SdOutEmmcBoot().run(ctx)


def test_sd_out_flag_with_a_root_that_is_not_the_emmc_refuses(tmp_path, monkeypatch):
    ctx, log, _ = _sd_out(tmp_path, monkeypatch, ["/dev/nvme0n1p2"], {"mmc0:0001": "MMC"},
                          sd_removed_by_operator=True)
    with pytest.raises(steps.Refused, match="not the eMMC"):
        steps.SdOutEmmcBoot().run(ctx)
    assert "cold" not in log


def test_sd_out_nosupport_on_a_new_protocol_is_a_real_fault(tmp_path, monkeypatch):
    ctx, log, _ = _sd_out(tmp_path, monkeypatch, ["/dev/mmcblk1p2"], {}, proto=(0, 15, 0),
                          sd_removed_by_operator=True)
    with pytest.raises(lt.Gd32NoSupport):
        steps.SdOutEmmcBoot().run(ctx)
    assert log == []


def test_sd_out_gd32_success_path_records_its_method(tmp_path, monkeypatch):
    log: list[str] = []
    monkeypatch.setattr(steps.lt, "root_device", lambda t: "/dev/mmcblk0p2")
    monkeypatch.setattr(steps.lt, "gd32_sd_out_flag_set", lambda t, bus, addr: 1)
    monkeypatch.setattr(steps.lt, "gd32_boot_config", lambda t, bus, f=None, addr=0x70: 1)
    monkeypatch.setattr(steps.lt, "sdio_mux_set", lambda t, c: log.append("mux"))
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(), execute=True)
    res = steps.SdOutEmmcBoot().run(ctx)
    assert res.status == "done" and res.evidence["sd_out_method"] == "gd32-boot-config" and log == ["mux"]


def _frame(*body: int) -> str:
    return " ".join(f"0x{b:02x}" for b in body)


def test_nosupport_parse_raises_the_dedicated_error_on_the_real_v030_frame():
    crc = lt._crc16_ccitt_false(b"\x06")                  # released v0.3.0: [status][crc lo][crc hi] + padding
    with pytest.raises(lt.Gd32NoSupport):
        lt.gd32_parse_boot_config(_frame(0x06, crc & 0xFF, crc >> 8, 0, 0, 0, 0))


def test_nosupport_with_a_bad_crc_is_a_transport_error_not_nosupport():
    crc = lt._crc16_ccitt_false(b"\x06")
    with pytest.raises(BenchError, match="bad reply CRC") as ei:
        lt.gd32_parse_boot_config(_frame(0x06, (crc & 0xFF) ^ 1, crc >> 8, 0, 0, 0, 0))
    assert not isinstance(ei.value, lt.Gd32NoSupport)


# --- census_final re-verifies the secure page read-only ------------------------------------

def _final(tmp_path, staged):
    board = Board(act_0x10=0x08)
    board.page[:] = bytes(range(64))                      # written, lock bit clear (lock=0xFD)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    if staged is not None:
        ctx.unit_dir.mkdir(parents=True, exist_ok=True)
        (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").write_bytes(staged)
    writes = []
    orig = board._answer

    def spy(cmd):
        if "w66@0x58" in cmd or "w3@0x58" in cmd:
            writes.append(cmd)
        return orig(cmd)
    board._answer = spy
    res = steps.run_steps(ctx, only=["census_final"])[-1]
    return res, writes


def test_census_final_verifies_the_secure_page_against_the_staged_blob(tmp_path):
    res, writes = _final(tmp_path, bytes(range(64)))
    assert res.evidence["secure_page_state"] == "written-verified", res.evidence
    assert writes == []                                    # never wrote or locked the page


def test_census_final_flags_a_page_that_differs_from_the_staged_blob(tmp_path):
    res, _ = _final(tmp_path, b"\x11" * 64)
    assert res.evidence["secure_page_state"].startswith("unread (secure page differs")


def test_census_final_without_a_staged_blob_stays_unread(tmp_path):
    res, _ = _final(tmp_path, None)
    assert res.evidence["secure_page_state"].startswith("unread (") and "staged.bin" in res.evidence["secure_page_state"]
