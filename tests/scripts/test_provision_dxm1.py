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
from pathlib import Path

import pytest
from provision import dxm1, steps
from provision import linux_target as lt
from provision.bench import BenchError, ExpectTimeout

from .test_provision_steps import DTB, FIP, Board, _bench, _bundle, _ctx

DXRT_CLI_S = Path(__file__).parent / "fixtures" / "provision" / "dxrt-cli-s.txt"

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


class Clock:
    """Fake time for dxm1: sleeping advances it, so a 400 s flash runs instantly."""

    def __init__(self):
        self.now = 0.0

    def sleep(self, s):
        self.now += s


class DxBoard(Board):
    """The unit: `pcie`/`fw` as dxrt-cli sees them, scripted dxflash outcome (rc, log, lands)."""

    def __init__(self, pcie="0x0001", fw=None, outcome=(0, OK_LOG, True), erase_out="", timeline=None,
                 clock=None, **kw):
        super().__init__(**kw)
        self.timeline, self.clock, self.start, self.killed = timeline, clock, 0.0, False
        self.pcie, self.fw, self.outcome, self.erase_out = pcie, fw, outcome, erase_out
        self.lands = False
        self.files[LIVE] = RELEASE_DTB
        self.gpio: list[str] = []
        self.events: list[str] = []
        self.corrupt: set[str] = set()      # remotes whose copy on the target is damaged
        self.fail_install = False           # the copy over the live DTB dies half way
        self.warm_exc: Exception | None = None
        self.stubborn = False               # dxflash survives every pkill
        self.uart = False                   # serial@12801000 is up (set by the warm reboot)

    def put(self, local, remote):
        self.events.append(f"put {remote}")
        super().put(local, remote)
        if remote in self.corrupt:
            self.files[remote] = b"damaged in transit"

    def _answer(self, cmd):
        if cmd.startswith("cat /sys/bus/pci/devices/0000:01:00.0/device"):
            return (0, self.pcie + "\n") if self.pcie else (1, "")
        if cmd == "dxrt-cli -s":
            return (0, DXRT_CLI_S.read_text(encoding="utf-8").replace("v2.4.0", f"v{self.fw}")) if self.fw else (1, "")
        if m := re.match(r"test -e (\S+)$", cmd):
            return (0 if m[1] in self.files else 1), ""
        if cmd == f"test -d {dxm1.UART_SYSFS}":
            return (0 if self.uart else 1), ""
        if m := re.match(r"cp (?:-p )?(\S+) (\S+) && sync$", cmd):
            if self.fail_install and m[1] == dxm1.REMOTE[dxm1.ROLE_DTB] and m[2] == LIVE:
                self.files[LIVE] = b"half-written"
                return 1, ""
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
        if m := re.match(r"echo (\d+) > /sys/class/gpio/unexport$", cmd):
            self.gpio.append(f"unexport {m[1]}")
            return 0, ""
        if cmd.startswith("rm -f /tmp/dxflash.log"):
            rc, log, self.lands = self.outcome
            self.files[dxm1.LOG], self.files[dxm1.RC] = log.encode(), f"{rc}\n".encode()
            self.events.append("dxflash started")
            self.start = self.clock.now if self.clock else 0.0
            return 0, ""
        el = (self.clock.now - self.start) if self.timeline else None
        if cmd.startswith("pkill"):
            self.killed = not self.stubborn
            return 0, ""
        if cmd.startswith("pgrep -f"):
            if self.stubborn:
                return 0, "1234"
            return (1, "") if self.killed or (el is not None and el >= self.timeline["rc_at"]) else (0, "1234")
        if cmd.startswith("cat /tmp/dxflash.rc"):
            if self.timeline and (self.killed or el < self.timeline["rc_at"]):
                return 0, ""                       # the process is still running
            return 0, self.files[dxm1.RC].decode()
        if cmd == "cat /tmp/dxflash.log":
            if self.timeline and el < self.timeline["marker_at"]:
                return 0, "SENT fw.bin 635896 B\n"
            return 0, self.files[dxm1.LOG].decode()
        if cmd.startswith("python3 -u /tmp/dxcli.py"):
            self.events.append("sf_erase")
            self.gpio.append("erase")
            self.dxcli_cmd = cmd
            return 0, self.erase_out or "40000000: ffffffff ffffffff ffffffff ffffffff\n"
        return super()._answer(cmd)


@pytest.fixture
def fake_boot(monkeypatch):
    """Replace the console-owning reboots; the unit state follows what the step did."""
    holder = {}

    def warm(ctx):
        b = holder["board"]
        b.events.append("warm reboot")
        if b.warm_exc:
            raise b.warm_exc
        for k in [k for k in b.files if k.startswith("/tmp/")]:
            del b.files[k]                      # /tmp is tmpfs on this image
        if b.files[LIVE] == DX_DTB:             # only the dxuart2 DTB enables the UART and disables PCIe
            b.pcie, b.uart = None, True

    def cold(ctx):
        b = holder["board"]
        b.events.append("poweroff + cold cycle")
        b.pcie, b.fw = ("0x0000", VERSION) if b.lands else ("0x0001", None)
        return ""

    monkeypatch.setattr(steps, "warm_reboot_to_linux", warm)
    monkeypatch.setattr(steps, "poweroff_and_cold_boot", cold)
    monkeypatch.setattr(lt, "_sysfs_gpio_dir", lambda t, label, line: f"/gpio{line}")
    monkeypatch.setattr(lt, "_pinctrl_chip_base", lambda t, label: 416)
    clock = Clock()
    holder["clock"] = clock
    monkeypatch.setattr(dxm1, "_clock", lambda: clock.now)
    monkeypatch.setattr(dxm1, "_sleep", clock.sleep)
    return holder


def _setup(tmp_path, holder, board=None, execute=True, **bundle_kw):
    board = board or DxBoard()
    holder["board"] = board
    ctx = _ctx(tmp_path, bundle=_dx_bundle(tmp_path, **bundle_kw), bench=_bench(), linux=board, execute=execute)
    return ctx, board


def _record(ctx, md5, via="unit"):
    """What the unit's records say the NAND holds: the ledger unit.yaml or the state file."""
    if via == "unit":
        ctx.unit_dir.mkdir(parents=True, exist_ok=True)
        (ctx.unit_dir / f"{ctx.serial}.unit.yaml").write_text(f"dxm1_fw_md5: {md5}\n", encoding="utf-8")
    else:
        ctx.state = {"steps": {"dxm1_npu_flash": {"status": "done", "evidence": {"dxm1_fw_md5": md5}}}}


def _run(ctx):
    return steps.run_steps(ctx, only=["dxm1_npu_flash"])[-1]


# --- probe ---------------------------------------------------------------------------

def test_probe_satisfied_when_pcie_is_0x0000_and_the_firmware_version_matches(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION))
    _record(ctx, _md5(FW))
    r = _run(ctx)
    assert r.status == "skipped"
    assert r.evidence == {"dxm1_fw_version": VERSION, "dxm1_fw_md5": _md5(FW),
                          "dxm1_fw_uart_boot_md5": _md5(UART_BOOT), "dxm1_pcie_device": "0x0000"}
    assert not any(c.startswith(("cp", "rm", "put")) for c in board.commands)


def test_probe_satisfied_from_the_state_file_record_too(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION))
    _record(ctx, _md5(FW), via="state")
    assert isinstance(steps.Dxm1NpuFlash().probe(ctx), steps.Satisfied)


OLD_FW_MD5 = "57eee8dd20ada70050491fd315f1f178"      # fw_no_pmic_gpio.bin: same v2.4.0, hangs on inference


@pytest.mark.parametrize("via", ["unit", "state"])
def test_same_version_but_a_different_recorded_md5_reflashes_through_the_erase_path(tmp_path, fake_boot, via):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION), cli=True)
    _record(ctx, OLD_FW_MD5, via=via)
    probe = steps.Dxm1NpuFlash().probe(ctx)
    assert isinstance(probe, steps.Unsatisfied) and OLD_FW_MD5 in probe.reason and _md5(FW) in probe.reason
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert board.events[:2] == ["put /tmp/dxuart2.dtb", "warm reboot"] and "sf_erase" in board.events
    assert r.evidence["dxm1_fw_md5"] == _md5(FW)


def test_no_recorded_md5_runs_the_step_even_when_pcie_and_version_match(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION), cli=True)
    probe = steps.Dxm1NpuFlash().probe(ctx)
    assert isinstance(probe, steps.Unsatisfied) and "no dxm1_fw_md5 recorded" in probe.reason
    assert _run(ctx).status == "done" and "sf_erase" in board.events


def test_force_step_runs_a_satisfied_step(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw=VERSION), cli=True)
    _record(ctx, _md5(FW))
    assert _run(ctx).status == "skipped"
    r = steps.run_steps(ctx, only=["dxm1_npu_flash"], force=["dxm1_npu_flash"])[-1]
    assert r.status == "done" and "dxflash started" in board.events


def test_erase_follows_the_proven_dx_update_sh_sequence(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0"), cli=True)
    assert _run(ctx).status == "done"
    assert board.dxcli_cmd == ("python3 -u /tmp/dxcli.py 'sf_erase 0 1000000@240' "
                               "'sf_read 0x40000000 0x200000 0x10@5' 'md 0x40000000 4'")
    assert board.gpio[:3] == ["/gpio61=high", "/gpio86=high", "erase"]        # SCI1 muxed, reset released first


def test_erase_no_prompt_fails_with_the_cold_cycle_advice_and_restores_the_dtb(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0", erase_out="NO PROMPT\n"), cli=True)
    r = _run(ctx)
    assert r.status == "failed" and "NO PROMPT" in r.detail and "cold-cycle" in r.detail
    assert "dxflash started" not in board.events and board.files[LIVE] == RELEASE_DTB


def test_erase_fails_with_the_readback_when_the_rtos_slot_is_not_blank(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0", erase_out="40000000: 12345678\n"), cli=True)
    r = _run(ctx)
    assert r.status == "failed" and "RTOS slot is not blank" in r.detail and "40000000: 12345678" in r.detail
    assert "dxflash started" not in board.events and board.files[LIVE] == RELEASE_DTB


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
    assert r.status == "failed" and "'version' is a required property" in r.detail   # the schema (preflight) rejects it first


# --- plan ----------------------------------------------------------------------------

def test_plan_lists_every_stage_and_touches_nothing(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, execute=False)
    r = _run(ctx)
    assert r.status == "planned", r.detail
    log = "\n".join(ctx.plan_log)
    for want in ("push the dxuart2 DTB to /tmp", "push dxm1_fw, dxm1_fw_uart_boot, dxm1_dxflash to /tmp AFTER the reboot", f"back up {LIVE} as .release",
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
    assert board.events == ["put /tmp/dxuart2.dtb", "warm reboot", "put /tmp/dx_fw.bin",
                            "put /tmp/dx_uart_boot.bin", "put /tmp/dxflash.py", "dxflash started",
                            "poweroff + cold cycle"]
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


def test_no_flash_payload_is_pushed_before_the_reboot_and_all_of_it_after(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0"), cli=True)
    assert _run(ctx).status == "done"
    reboot = board.events.index("warm reboot")
    before = [e for e in board.events[:reboot] if e.startswith("put")]
    after = {e for e in board.events[reboot:] if e.startswith("put")}
    assert before == ["put /tmp/dxuart2.dtb"]        # the DTB is copied into /boot before the reboot
    assert after == {"put /tmp/dx_fw.bin", "put /tmp/dx_uart_boot.bin", "put /tmp/dxflash.py", "put /tmp/dxcli.py"}


def test_reflash_of_a_running_unit_erases_the_nand_first(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0", erase_out="sf_erase ok\n40000000: ffffffff\n"), cli=True)
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert board.events == ["put /tmp/dxuart2.dtb", "warm reboot", "put /tmp/dx_fw.bin",
                            "put /tmp/dx_uart_boot.bin", "put /tmp/dxflash.py", "put /tmp/dxcli.py",
                            "sf_erase", "dxflash started", "poweroff + cold cycle"]    # dxcli pushed before its use
    assert r.evidence["dxm1_nand_erase"] == "NAND erased, RTOS slot blank (sf_erase 0 1000000)"


def test_wrong_strap_fails_with_the_rework_message_and_restores_the_dtb(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(outcome=(1, STRAP_LOG, False)))
    r = _run(ctx)
    assert r.status == "failed"
    for want in ("BOOT_CFG straps are not mode 0", "IO17, IO19 and IO20", "carrier needs the rework",
                 "release DTB restored"):
        assert want in r.detail, want
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert "poweroff + cold cycle" not in board.events
    assert board.gpio[-2:] == ["unexport 477", "unexport 502"] and "P75/PA6 unexported" in r.detail


def test_dxflash_failure_reports_the_real_exit_code_and_restores_the_dtb(tmp_path, fake_boot):
    # XMODEM was offered (so the strap is fine) but the transfer died: not a strap message
    log = "ROM: XMODEM C\nsend boot\ntimeout waiting for ACK\n"
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(outcome=(3, log, False)))
    r = _run(ctx)
    assert r.status == "failed"
    assert "exit code 3" in r.detail and "timeout waiting for ACK" in r.detail and "straps" not in r.detail
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert board.events == ["put /tmp/dxuart2.dtb", "warm reboot", "put /tmp/dx_fw.bin",
                            "put /tmp/dx_uart_boot.bin", "put /tmp/dxflash.py", "dxflash started"]


def _slow(fake_boot, marker_at, rc_at, outcome=(0, OK_LOG, True)):
    return DxBoard(outcome=outcome, clock=fake_boot["clock"], timeline={"marker_at": marker_at, "rc_at": rc_at})


def test_a_flash_that_finishes_after_300_s_is_a_success(tmp_path, fake_boot):
    # dxflash sits out a 240 s log window after SENT fw.bin: markers at ~395 s, exit at ~400 s
    ctx, board = _setup(tmp_path, fake_boot, _slow(fake_boot, 395, 400))
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert fake_boot["clock"].now > 300 and r.evidence["dxm1_dxflash_rc"] == "0"
    assert not any(c.startswith("pkill") for c in board.commands)       # it exited by itself


def test_markers_then_a_lingering_process_is_a_success_and_the_flasher_is_stopped(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, _slow(fake_boot, 100, 10**9))
    r = _run(ctx)
    assert r.status == "done", r.detail
    assert r.evidence["dxm1_dxflash_rc"] == "killed after its success markers"
    assert any(c.startswith("pkill -f") for c in board.commands)


def test_timeout_kills_dxflash_before_the_gpios_and_the_dtb_are_touched(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot, _slow(fake_boot, 10**9, 10**9, (0, OK_LOG, False)))
    r = _run(ctx)
    assert r.status == "failed"
    assert "did not finish in 600 s (killed)" in r.detail and "SENT fw.bin 635896 B" in r.detail
    assert fake_boot["clock"].now >= 600
    cmds = board.commands
    kill = next(i for i, c in enumerate(cmds) if c.startswith("pkill -f '[/]tmp/dxflash.py'"))
    assert any(c.startswith("pgrep -f") for c in cmds[kill:])           # waited for it to be gone
    unexport = next(i for i, c in enumerate(cmds) if c.endswith("/sys/class/gpio/unexport"))
    restore = next(i for i, c in enumerate(cmds) if c.startswith(f"cp -p {BAK} {LIVE}"))
    assert kill < unexport < restore
    assert board.files[LIVE] == RELEASE_DTB and "poweroff + cold cycle" not in board.events


def test_a_bad_crc_fails_at_once_and_kills_the_flasher(tmp_path, fake_boot):
    log = "SENT fw.bin 635896 B\nupdate_firmware end. 0\nbad CRC: 0x1 vs 0x2\njump to rtos\n"
    ctx, board = _setup(tmp_path, fake_boot, _slow(fake_boot, 0, 10**9, (0, log, False)))
    r = _run(ctx)
    assert r.status == "failed" and "bad CRC" in r.detail and fake_boot["clock"].now < 100
    assert any(c.startswith("pkill -f") for c in board.commands)


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


def test_a_failed_copy_over_the_live_dtb_still_restores_the_release_dtb(tmp_path, fake_boot):
    board = DxBoard()
    board.fail_install = True
    ctx, board = _setup(tmp_path, fake_boot, board)
    r = _run(ctx)
    assert r.status == "failed" and "release DTB restored" in r.detail
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert "warm reboot" not in board.events


def test_a_warm_reboot_timeout_fails_the_step_cleanly_and_heals_the_dtb(tmp_path, fake_boot):
    board = DxBoard()
    board.warm_exc = ExpectTimeout("login:", "tail", 240.0)
    ctx, board = _setup(tmp_path, fake_boot, board)
    r = _run(ctx)                                    # must not raise a TypeError out of run_one
    assert r.status == "failed" and "no match for 'login:'" in r.detail and "release DTB restored" in r.detail
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files
    assert ctx.state["steps"]["dxm1_npu_flash"]["status"] != "running"


def test_the_dxuart2_dtb_must_have_booted_before_any_gpio_or_erase(tmp_path, fake_boot, monkeypatch):
    ctx, board = _setup(tmp_path, fake_boot, DxBoard(pcie="0x0000", fw="2.3.0"), cli=True)
    monkeypatch.setattr(steps, "warm_reboot_to_linux", lambda c: board.events.append("warm reboot"))   # U-Boot loaded the release DTB
    r = _run(ctx)
    assert r.status == "failed" and "the dxuart2 DTB did not boot" in r.detail and "12801000.serial" in r.detail
    assert "sf_erase" not in board.events and "dxflash started" not in board.events and board.gpio == []
    assert board.files[LIVE] == RELEASE_DTB


def test_pcie_still_enumerated_after_the_reboot_means_the_dxuart2_dtb_did_not_boot():
    board = DxBoard(pcie="0x0000")
    board.uart = True
    with pytest.raises(BenchError, match=r"still enumerated"):
        dxm1.check_dxuart2_booted(board)


@pytest.mark.parametrize("cli", [False, True])
def test_a_damaged_payload_copy_aborts_before_the_erase_and_the_flash(tmp_path, fake_boot, cli):
    board = DxBoard(pcie="0x0000" if cli else "0x0001", fw="2.3.0" if cli else None)
    board.corrupt = {dxm1.REMOTE[dxm1.ROLE_UART_BOOT]}
    ctx, board = _setup(tmp_path, fake_boot, board, cli=cli)
    r = _run(ctx)
    assert r.status == "failed" and "/tmp/dx_uart_boot.bin" in r.detail and "does not match" in r.detail
    assert "sf_erase" not in board.events and "dxflash started" not in board.events and board.gpio == []
    assert board.files[LIVE] == RELEASE_DTB and BAK not in board.files


def test_a_flasher_that_survives_pkill_9_is_reported_and_the_gpios_stay_exported(tmp_path, fake_boot):
    board = _slow(fake_boot, 100, 10**9)
    board.stubborn = True
    ctx, board = _setup(tmp_path, fake_boot, board)
    r = _run(ctx)
    assert r.status == "failed" and "flasher still alive" in r.detail
    assert not any(g.startswith("unexport") for g in board.gpio)
    assert board.files[LIVE] == RELEASE_DTB


def test_the_dxflash_rc_evidence_is_always_a_real_string():
    assert dxm1.rc_evidence(0, "", False) == "0"
    assert dxm1.rc_evidence(None, OK_LOG, False) == "killed after its success markers"
    assert dxm1.rc_evidence(None, "SENT", True) == "killed on timeout"
    assert dxm1.rc_evidence(None, "bad CRC", False) == "killed (bad CRC)"


def test_a_leftover_release_dtb_makes_census_refuse(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot)
    board.files[BAK] = RELEASE_DTB
    r = steps.run_steps(ctx, only=["census"])[-1]
    assert r.status == "failed" and BAK in r.detail and "interrupted dxm1_npu_flash" in r.detail


def test_a_leftover_release_dtb_makes_boot_sd_linux_refuse(tmp_path, fake_boot):
    ctx, board = _setup(tmp_path, fake_boot)
    board.files[BAK] = RELEASE_DTB
    r = steps.run_steps(ctx, only=["boot_sd_linux"])[-1]
    assert r.status == "failed" and BAK in r.detail


def test_the_recorded_md5_does_not_fall_back_past_a_failed_current_entry(tmp_path, fake_boot):
    ctx, _ = _setup(tmp_path, fake_boot)
    _record(ctx, _md5(FW))                           # the ledger holds an older good record
    ctx.state = {"steps": {"dxm1_npu_flash": {"status": "failed"}},
                 "superseded": [{"steps": {"dxm1_npu_flash": {"status": "done", "evidence": {"dxm1_fw_md5": _md5(FW)}}}}]}
    assert steps.Dxm1NpuFlash._recorded_md5(ctx) == ""


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
        dxm1.gpio_config(raw, V2M_PRESET)


V2M_PRESET = {"on_module": {"dxm1": {"uart_mux_pin": "P75", "reset_pin": "PA6", "gpio_chip": "10410000.pinctrl"}}}


def test_gpio_config_resolves_the_presets_pins_p75_and_pa6():
    assert dxm1.gpio_config({}, V2M_PRESET) == ("10410000.pinctrl", 61, 86)


def test_gpio_config_needs_a_dxm1_preset():
    with pytest.raises(ValueError, match="no on_module.dxm1"):
        dxm1.gpio_config({}, {"on_module": {}})
