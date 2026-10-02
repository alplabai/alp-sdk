"""provision.dxm1 + steps.Dxm1NpuFlash: the DX-M1 NPU firmware step against a fake target.

The fake models what the step observes on the unit: /boot DTB files, the PCIe device id
and `dxrt-cli -s` firmware version (which change only when a scripted dxflash run
"lands" the firmware and the unit is cold-booted), the background dxflash log / exit
code files, and the sysfs GPIO writes. The reboots (console owned) are replaced by
recorders, as in the other step tests.
"""

from __future__ import annotations

import hashlib
import json
import re

import pytest
from provision import dxm1, steps
from provision import linux_target as lt
from provision.bench import BenchError

from .test_provision_steps import DTB, FIP, Board, _bench, _bundle, _ctx

FW = b"dx-fw-image" * 8
UART_BOOT = b"dx-uart-boot" * 8
DXFLASH = b"#!/usr/bin/env python3\n# dxflash\n"
DXCLI = b"#!/usr/bin/env python3\n# dxcli\n"
DX_DTB = b"dxuart2-dtb-blob"
RELEASE_DTB = b"release-dtb-blob"
LIVE, BAK = f"/boot/{DTB}", f"/boot/{DTB}.release"
VERSION = "2.4.0"
OK_LOG = "ROM: XMODEM C\nsend boot\nupdate_firmware end. 0\ngood CRC\njump to rtos\n"
STRAP_LOG = "".join(f"pcie boot({i}/3) failed (0x1) PWD:\n" for i in (1, 2, 3))


def _md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


def _dx_bundle(tmp_path, family="v2n-m1", roles=("fw", "boot", "flash", "dtb"), version=VERSION, cli=False):
    bdir, b = _bundle(tmp_path, family=family)
    table = {"fw": (dxm1.ROLE_FW, "fw.bin", FW), "boot": (dxm1.ROLE_UART_BOOT, "fw_uart_boot.bin", UART_BOOT),
             "flash": (dxm1.ROLE_DXFLASH, "dxflash.py", DXFLASH), "dtb": (dxm1.ROLE_DTB, "dxuart2.dtb", DX_DTB),
             "cli": (dxm1.ROLE_DXCLI, "dxcli.py", DXCLI)}
    for key in (*roles, *(["cli"] if cli else [])):
        role, name, data = table[key]
        (bdir / "artifacts" / name).write_bytes(data)
        comp = {"role": role, "file": f"artifacts/{name}", "sha256": hashlib.sha256(data).hexdigest(),
                "size_bytes": len(data), "flash_target": "dxm1"}
        if role == dxm1.ROLE_FW and version:
            comp["version"] = version
        b["components"].append(comp)
    (bdir / "bundle.json").write_text(json.dumps(b), encoding="utf-8")
    return bdir, b


class DxBoard(Board):
    """The unit: `pcie`/`fw` as dxrt-cli sees them, scripted dxflash outcome (rc, log, lands)."""

    def __init__(self, pcie="0x0001", fw=None, outcome=(0, OK_LOG, True), erase_out="", **kw):
        super().__init__(**kw)
        self.pcie, self.fw, self.outcome, self.erase_out = pcie, fw, outcome, erase_out
        self.lands = False
        self.files[LIVE] = RELEASE_DTB
        self.gpio: list[str] = []
        self.events: list[str] = []

    def _answer(self, cmd):
        if cmd.startswith("cat /sys/bus/pci/devices/0000:01:00.0/device"):
            return (0, self.pcie + "\n") if self.pcie else (1, "")
        if cmd == "dxrt-cli -s":
            return (0, f"Firmware version : v{self.fw}\n") if self.fw else (1, "")
        if m := re.match(r"test -e (\S+)$", cmd):
            return (0 if m[1] in self.files else 1), ""
        if m := re.match(r"cp (?:-p )?(\S+) (\S+) && sync$", cmd):
            self.files[m[2]] = self.files[m[1]]
            return 0, ""
        if m := re.match(r"rm -f (\S+) && sync$", cmd):
            self.files.pop(m[1], None)
            return 0, ""
        if cmd == "sync":
            return 0, ""
        if m := re.match(r"echo (high|low) > (\S+)/direction( && sleep ([\d.]+) && echo high > \S+/direction)?$", cmd):
            self.gpio.append(f"{m[2]}={m[1]}" + (f" {m[4]}s high" if m[3] else ""))
            return 0, ""
        if cmd.startswith("rm -f /tmp/dxflash.log"):
            rc, log, self.lands = self.outcome
            self.files[dxm1.LOG], self.files[dxm1.RC] = log.encode(), f"{rc}\n".encode()
            self.events.append("dxflash started")
            return 0, ""
        if cmd.startswith("cat /tmp/dxflash.rc"):
            return 0, self.files[dxm1.RC].decode()
        if cmd == "cat /tmp/dxflash.log":
            return 0, self.files[dxm1.LOG].decode()
        if cmd.startswith("python3 /tmp/dxcli.py"):
            self.events.append("sf_erase")
            return (0, self.erase_out) if self.erase_out else (0, "NO PROMPT")
        return super()._answer(cmd)


@pytest.fixture
def fake_boot(monkeypatch):
    """Replace the console-owning reboots; the unit state follows what the step did."""
    holder = {}

    def warm(ctx):
        b = holder["board"]
        b.events.append("warm reboot")
        b.pcie = None                           # dxuart2 DTB: pcie@13400000 disabled

    def cold(ctx):
        b = holder["board"]
        b.events.append("poweroff + cold cycle")
        b.pcie, b.fw = ("0x0000", VERSION) if b.lands else ("0x0001", None)
        return ""

    monkeypatch.setattr(steps, "warm_reboot_to_linux", warm)
    monkeypatch.setattr(steps, "poweroff_and_cold_boot", cold)
    monkeypatch.setattr(lt, "_sysfs_gpio_dir", lambda t, label, line: f"/gpio{line}")
    return holder


def _setup(tmp_path, holder, board=None, execute=True, **bundle_kw):
    board = board or DxBoard()
    holder["board"] = board
    ctx = _ctx(tmp_path, bundle=_dx_bundle(tmp_path, **bundle_kw), bench=_bench(), linux=board, execute=execute)
    return ctx, board


def _run(ctx):
    return steps.run_steps(ctx, only=["dxm1_npu_flash"])[-1]


# --- probe ---------------------------------------------------------------------------

def test_probe_satisfied_when_pcie_is_0x0000_and_the_firmware_version_matches(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION))
    r = _run(ctx)
    assert r.status == "skipped"
    assert r.evidence == {"dxm1_fw_version": VERSION, "dxm1_fw_md5": _md5(FW),
                          "dxm1_fw_uart_boot_md5": _md5(UART_BOOT), "dxm1_pcie_device": "0x0000"}
    assert not any(c.startswith(("cp", "rm", "put")) for c in board.commands)


@pytest.mark.parametrize("pcie, fw, why", [
    ("0x0001", None, "device 0x0001"),          # ROM PCIe boot: no firmware on the NAND
    (None, None, "device absent"),
    ("0x0000", "2.3.0", "firmware 2.3.0 != bundle 2.4.0"),
    ("0x0000", None, "firmware unreadable"),
])
def test_probe_unsatisfied_never_skips_a_needed_flash(tmp_path, fake_boot, pcie, fw, why):
    ctx, _ = _setup(tmp_path, fake_boot, DxBoard(pcie=pcie, fw=fw, erase_out="x"), cli=True)
    probe = steps.Dxm1NpuFlash().probe(ctx)
    assert isinstance(probe, steps.Unsatisfied) and why in probe.reason


def test_skipped_without_dx_artefacts_or_on_a_non_deepx_family(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot, roles=("fw", "boot"))
    r = _run(ctx)
    assert r.status == "skipped" and "missing dxm1_dxflash, dxm1_dtb" in r.detail
    ctx, _ = _setup(tmp_path / "v2n", fake_boot, family="v2n")
    assert "not a V2M/DEEPX SKU" in steps.Dxm1NpuFlash().run(ctx).detail


def test_the_bundle_must_declare_the_expected_firmware_version(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot, version=None)
    r = _run(ctx)
    assert r.status == "failed" and "declares no version" in r.detail


# --- plan ----------------------------------------------------------------------------

def test_plan_lists_every_stage_and_touches_nothing(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, execute=False)
    r = _run(ctx)
    assert r.status == "planned", r.detail
    log = "\n".join(ctx.plan_log)
    for want in ("push dxm1_fw, dxm1_fw_uart_boot, dxm1_dxflash, dxm1_dtb", f"back up {LIVE} as .release",
                 "warm reboot onto the dxuart2 DTB", "P75 + PA6 high, dxflash.py in the background",
                 "PA6 low 0.5 s after 2 s", f"restore {LIVE} from .release", "clean poweroff, cold cycle",
                 "verify PCIe 0000:01:00.0 device 0x0000"):
        assert want in log, want
    assert "sf_erase" not in log                      # PCIe 0x0001: blank NAND
    assert board.events == [] and not any(re.match(r"cp|rm|echo|python3|nohup", c) for c in board.commands)
    assert board.files[LIVE] == RELEASE_DTB


def test_plan_erases_first_when_boot2nd_already_runs_and_needs_dxcli(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0"), execute=False)
    r = _run(ctx)
    assert r.status == "failed" and "dxm1_dxcli" in r.detail
    ctx, _ = _setup(tmp_path / "cli", fake_boot, DxBoard(pcie="0x0000", fw="2.3.0"), execute=False, cli=True)
    r = _run(ctx)
    assert r.status == "planned" and "sf_erase 0 1000000 via dxcli.py" in "\n".join(ctx.plan_log)


# --- execute -------------------------------------------------------------------------

def test_success_path(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot)
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert r.evidence["dxm1_fw_version"] == VERSION and r.evidence["dxm1_dxflash_rc"] == "0"
    assert r.evidence["dxm1_fw_md5"] == _md5(FW) and r.evidence["dxm1_fw_uart_boot_md5"] == _md5(UART_BOOT)
    assert r.evidence["dxm1_pcie_device"] == "0x0000"
    # order: reboot onto dxuart2, flash, restore, power-off + cold cycle
    assert board.events == ["warm reboot", "dxflash started", "poweroff + cold cycle"]
    # P75 + PA6 high, then PA6 pulsed low 0.5 s and back high after the 2 s wait
    assert board.gpio == ["/gpio61=high", "/gpio86=high", "/gpio86=low 0.5s high"]
    assert "sleep 2" in board.commands and board.commands.index("sleep 2") < board.commands.index(
        "echo low > /gpio86/direction && sleep 0.5 && echo high > /gpio86/direction")
    # release DTB back, md5 verified, backup gone; the dxuart2 DTB was installed in between
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert any(c == f"cp /tmp/dxuart2.dtb {LIVE} && sync" for c in board.commands)
    assert board.files["/tmp/dx_fw.bin"] == FW and board.files["/tmp/dxflash.py"] == DXFLASH
    assert "sf_erase" not in board.events
    assert OK_LOG in ctx.step_logs["dxm1_npu_flash"]


def test_reflash_of_a_running_unit_erases_the_nand_first(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0", erase_out="erased\n"), cli=True)
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert board.events == ["warm reboot", "sf_erase", "dxflash started", "poweroff + cold cycle"]
    assert r.evidence["dxm1_nand_erase"] == "NAND erased (sf_erase 0 1000000)"


def test_wrong_strap_fails_with_the_rework_message_and_restores_the_dtb(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(outcome=(1, STRAP_LOG, False)))
    r = _run(ctx)
    assert r.status == "failed"
    for want in ("BOOT_CFG straps are not mode 0", "IO17, IO19 and IO20", "carrier needs the rework",
                 "release DTB restored"):
        assert want in r.detail, want
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert "poweroff + cold cycle" not in board.events


def test_dxflash_failure_reports_the_real_exit_code_and_restores_the_dtb(tmp_path, fake_boot):
    # XMODEM was offered (so the strap is fine) but the transfer died: not a strap message
    log = "ROM: XMODEM C\nsend boot\ntimeout waiting for ACK\n"
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(outcome=(3, log, False)))
    r = _run(ctx)
    assert r.status == "failed"
    assert "exit code 3" in r.detail and "timeout waiting for ACK" in r.detail and "straps" not in r.detail
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert board.events == ["warm reboot", "dxflash started"]


def test_a_zero_exit_without_the_end_marker_is_still_a_failure(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot, DxBoard(outcome=(0, "ROM: XMODEM C\nsend boot\n", False)))
    r = _run(ctx)
    assert r.status == "failed" and "no 'update_firmware end. 0' with good CRC" in r.detail


def test_verify_fails_when_the_cold_boot_shows_the_rom_pcie_boot_device(tmp_path, fake_boot):
    # dxflash reported success but the firmware did not land: PCIe device stays 0x0001
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(outcome=(0, OK_LOG, False)))
    r = _run(ctx)
    assert r.status == "failed" and "0x0001" in r.detail and "no firmware on the NAND" in r.detail
    assert board.files[LIVE] == RELEASE_DTB


def test_verify_fails_on_a_firmware_version_mismatch(tmp_path, fake_boot, monkeypatch):
    ctx, board = _setup(tmp_path, fake_boot)
    monkeypatch.setattr(steps, "poweroff_and_cold_boot", lambda c: setattr(board, "pcie", "0x0000")
                        or setattr(board, "fw", "2.3.9") or "")
    r = _run(ctx)
    assert r.status == "failed" and "firmware 2.3.9 != bundle 2.4.0" in r.detail


# --- dxm1 helpers ----------------------------------------------------------------------

def test_install_dtb_heals_an_interrupted_run_instead_of_backing_up_over_the_release(tmp_path):
    board = DxBoard()
    board.files[LIVE] = DX_DTB                       # a previous run died with dxuart2 installed
    board.files[BAK] = RELEASE_DTB
    board.files["/tmp/dxuart2.dtb"] = DX_DTB
    new = tmp_path / "dxuart2.dtb"
    new.write_bytes(DX_DTB)
    assert dxm1.install_dtb(board, DTB, new) == _md5(RELEASE_DTB)
    assert board.files[BAK] == RELEASE_DTB and board.files[LIVE] == DX_DTB


def test_install_dtb_refuses_when_the_live_dtb_already_is_dxuart2_with_no_backup(tmp_path):
    board = DxBoard()
    board.files[LIVE] = DX_DTB
    new = tmp_path / "dxuart2.dtb"
    new.write_bytes(DX_DTB)
    with pytest.raises(BenchError, match="already is the dxuart2 DTB"):
        dxm1.install_dtb(board, DTB, new)


def test_restore_dtb_names_the_backup_when_the_md5_does_not_match(tmp_path):
    board = DxBoard()
    board.files[BAK] = b"corrupt"
    with pytest.raises(BenchError, match="restore FAILED.*until .*release is copied back"):
        dxm1.restore_dtb(board, DTB, _md5(RELEASE_DTB))


def test_classify_wrong_strap_needs_pcie_failures_and_no_xmodem():
    with pytest.raises(dxm1.StrapError):
        dxm1.classify(1, STRAP_LOG)
    with pytest.raises(BenchError) as e:
        dxm1.classify(1, STRAP_LOG + "XMODEM C\n")      # the ROM did offer XMODEM
    assert not isinstance(e.value, dxm1.StrapError)


@pytest.mark.parametrize("raw, match", [
    ({"dxm1": {"uart_mux_line": 52}}, "P64"),
    ({"dxm1": {"reset_line": 53}}, "P65"),
    ({"dxm1": {"uart_mux_line": 61, "reset_line": 61}}, "must be distinct"),
    ({"dxm1": {"reset_line": "86"}}, "must be an int"),
])
def test_gpio_config_refuses_bad_lines(raw, match):
    with pytest.raises(ValueError, match=match):
        dxm1.gpio_config(raw)


def test_gpio_config_defaults_are_p75_and_pa6():
    assert dxm1.gpio_config({}) == ("10410000.pinctrl", 61, 86)
