"""provision.steps: the V2N step machine against fakes (no hardware).

Board states replayed here:
  * blank board   -- nothing answers yet; the plan walks every step as WOULD.
  * board #1      -- Linux up, xSPI holds the release BL2 but an older FIP
                     without the DEEPX rail string, EEPROM blank: the plan
                     must refuse eeprom_manifest on the written xSPI bytes.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import re
from pathlib import Path

import pytest
import yaml
from provision import gates, ledger_out, steps
from provision import linux_target as lt
from provision.bench import Bench

from .provision_fakes import FakeConsole, FakeLinux, FakeOperator, FakePower, FakeProbe
from .test_provision_gates import _ext4, _wic

REPO = Path(__file__).resolve().parents[2]
SKU = "E1M-V2M103"
SERIAL = "2026W38-0001"
DTB = "e1m-test-evk.dtb"
# synthetic tier markers (the real table is private)
MARKERS = {
    "markers": [{"label": "T8", "hex": "a1a2a3a4a5a6a7a8", "dram_mbit": 32768},
                {"label": "T16", "hex": "b1b2b3b4b5b6b7b8", "dram_mbit": None}],
    "sku_tier": {SKU: {"label": "T8", "dram_mbit": 32768},
                 "E1M-V2N101": {"label": "T16", "dram_mbit": 32768}},
}
CATALOGUE = {
    "schema": 1, "family": "v2n",
    "keys": {
        "eeprom_unique_id": {"group": "identity", "source": "0x58", "mode": "auto", "ship_required": True},
        # act88760_gpio4_defect: legacy, no longer written by the tool; kept here only
        # so an existing unit.yaml carrying it still round-trips as informational.
        "act88760_gpio4_defect": {"group": "power", "source": "0x25", "mode": "auto", "ship_required": False},
        "act88760_gpio4_otp": {"group": "power", "source": "0x25", "mode": "auto", "ship_required": False},
        "act88760_gpio4_workaround": {"group": "power", "source": "", "mode": "auto", "ship_required": False},
        "act88760_gpio4_after_boot": {"group": "power", "source": "0x25", "mode": "auto", "ship_required": False},
        "gd32_dp_id": {"group": "gd32", "source": "", "mode": "auto", "ship_required": False},
        "disposition": {"group": "disposition", "source": "operator", "mode": "manual", "ship_required": True},
        "notes": {"group": "disposition", "source": "operator", "mode": "manual", "ship_required": False},
    },
}
BL2 = b"\x00" * 64 + bytes.fromhex(MARKERS["markers"][0]["hex"]) + b"\x11" * 64
FIP = b"\x00" * 64 + f"ext4load mmc 0:2 0x48000000 boot/{DTB}\0".encode() + gates.RAIL_PG.encode() + b"\x22" * 64
OLD_FIP = b"\x00" * 64 + f"boot/{DTB}\0".encode() + b"\x33" * 96


def _hx(data: bytes) -> str:
    return " ".join(f"0x{b:02x}" for b in data)


def _bundle(tmp_path, bl2=BL2, fip=FIP, sku=SKU, family="v2n-m1"):
    d = tmp_path / "bundle"
    (d / "artifacts").mkdir(parents=True)
    files = {"bl2": ("bl2_bp_spi.bin", bl2, "xspi:mtd0"), "bl2_mmc": ("bl2_bp_mmc.bin", bl2, "emmc:boot1"),
             "fip": ("fip.bin", fip, "xspi:mtd1"),
             "system_image": ("img.wic.gz", gzip.compress(_wic(_ext4(["Image", DTB])), mtime=0), "emmc")}
    comps = []
    for role, (name, data, target) in files.items():
        (d / "artifacts" / name).write_bytes(data)
        comps.append({"role": role, "file": f"artifacts/{name}", "sha256": hashlib.sha256(data).hexdigest(),
                      "size_bytes": len(data), "flash_target": target})
    bundle = {"schema_version": 1, "sku": sku, "family": family, "hw_rev": "r1",
              "release_version": "som-9.9.9", "created": "2026-09-24", "status": "complete",
              "memory_tier": {"dram_mbit": 32768, "label": "T8"}, "components": comps,
              "provenance": {"toolchain": "gcc", "u_boot_srcrev": "0", "tf_a_srcrev": "0",
                             "uboot_defconfig": "rzv2n-dev_defconfig", "patches": [], "equivalence": "n/a"}}
    (d / "bundle.json").write_text(json.dumps(bundle), encoding="utf-8")
    return d, bundle


def _ledger(tmp_path):
    root = tmp_path / "ledger"
    (root / "schema").mkdir(parents=True)
    (root / "schema" / "v2n.keys.yaml").write_text(yaml.safe_dump(CATALOGUE), encoding="utf-8")
    return root


def _bench(console=None, probe=None, operator=None):
    return Bench(console=console or FakeConsole([]), power=FakePower(), probe=probe or FakeProbe(),
                 operator=operator or FakeOperator(), linux_user="root", linux_host=None,
                 i2c_bus={"eeprom": 0, "pmic": 8, "brd": 8},
                 scif={"flash_writer": Path("writer.mot"), "baud": 115200,
                       "program_start": {"bl2_mmc": 0x1000, "fip": 0x2000}}, raw={})


def _failed(res):
    return next(r for r in res if r.status == "failed")


def _gd32_fw(tmp_path):
    d = tmp_path / "gd32"
    d.mkdir()
    for i, (name, _a, _k) in enumerate(steps.GD32_IMAGES):
        (d / name).write_bytes(bytes([i + 1]) * 32)
    return d


def _ctx(tmp_path, bundle=None, **kw):
    bdir, b = bundle or _bundle(tmp_path)
    preset = yaml.safe_load((REPO / "metadata" / "e1m_modules" / f"{kw.pop('preset_sku', SKU)}.yaml")
                            .read_text(encoding="utf-8"))
    kw.setdefault("tier_markers", MARKERS)
    kw.setdefault("expected_registers", {"devices": {}})
    return steps.Ctx(sku=kw.pop("sku", SKU), serial=SERIAL, bundle_dir=bdir, bundle=b, preset=preset,
                     ledger_root=kw.pop("ledger_root", _ledger(tmp_path)), **kw)


class Board(FakeLinux):
    """A stateful Linux target: block devices are byte strings (md5 via files),
    I2C registers live in `regs`, EEPROM array and identity header are modelled,
    and every write command mutates the model so re-probes see the result."""

    def __init__(self, xspi_bl2=b"", xspi_fip=b"", boot1=b"", emmc=b"", act_0x10=0x08,
                 lock=0xFD, rail_on_xspi=None, root="mmcblk1p2"):
        super().__init__()
        self.files = {"/dev/mtd0": xspi_bl2 + b"\xff" * 64, "/dev/mtd1": xspi_fip + b"\xff" * 64,
                      "/dev/mmcblk0boot1": boot1 or b"\x00" * (0x300 * 512 + 1024),
                      "/dev/mmcblk0": emmc or b"\x00" * 1024}
        self.regs = {(8, 0x25, 0x10): act_0x10}
        self.array = bytearray(b"\xff" * 128)
        self.page = bytearray(b"\xff" * 64)
        self.lock = lock
        self.ext = {177: 0x00, 179: 0x00}
        self.root = root
        self.rail = rail_on_xspi

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None):
        self.commands.append(cmd)
        rc, out = self._answer(cmd)
        res = lt.CmdResult(rc, out, "" if rc == 0 else "fake: unscripted")
        if check and rc != 0:
            raise lt.BenchError(f"{cmd!r} failed ({rc})")
        return res

    def _answer(self, cmd):
        if cmd == "true":
            return 0, ""
        if cmd.startswith("for d in /sys/block/mmcblk*"):
            return 0, "mmcblk0 MMC\nmmcblk1 SD\n"
        if cmd == "mountpoint -d /":
            return 0, "179:98\n"
        if cmd.startswith("readlink -f /sys/dev/block/"):
            return 0, f"/sys/devices/platform/soc/mmc/block/mmcblk1/{self.root}\n"
        if re.match(r"cat /sys/class/mtd/mtd\d/erasesize", cmd):
            return 0, "65536\n"
        if re.match(r"cat /sys/class/mtd/mtd\d/size", cmd):
            return 0, f"{0x800000}\n"
        if m := re.match(r"dd if=/dev/mtd1 bs=4096 count=(\d+) 2>/dev/null \| grep", cmd):
            has = gates.RAIL_PG.encode() in self.files["/dev/mtd1"][: int(m[1]) * 4096]
            return (0 if (has if self.rail is None else self.rail) else 1), ""
        if cmd.startswith("mmc extcsd read"):
            return 0, f"[BOOT_BUS_CONDITIONS: 0x{self.ext[177]:02x}]\n[PARTITION_CONFIG: 0x{self.ext[179]:02x}]\n"
        if m := re.match(r"i2cget -y -f (\d+) (0x\w+) (0x\w+)", cmd):
            key = (int(m[1]), int(m[2], 16), int(m[3], 16))
            return (0, f"0x{self.regs[key]:02x}\n") if key in self.regs else (1, "")
        if m := re.match(r"i2cset -y (\d+) (0x\w+) (0x\w+) (0x\w+)", cmd):
            self.regs[(int(m[1]), int(m[2], 16), int(m[3], 16))] = int(m[4], 16)
            return 0, ""
        if cmd.startswith("i2cdetect -y -r"):
            return 0, "70: 70 -- -- -- -- -- -- --\n"
        if cmd.startswith("i2ctransfer -f -y 8 w4@0x70 0x00 0x01 "):
            return 0, "0x00 0x00 0x0d 0x00 0x9c 0xf2\n"   # GET_VERSION: OK, protocol 0.13.0 (bench bytes)
        if cmd.startswith("sleep"):
            return 0, ""
        if m := re.match(r"i2ctransfer -y 0 (.*)$", cmd):
            return self._i2c(m[1])
        return 1, ""   # census and anything else unscripted: a failed read, never a write

    def _i2c(self, msgs):
        if msgs == "w2@0x58 0x04 0x00 r1":
            return 0, f"0x{self.lock:02x}"
        if msgs == "w2@0x58 0x02 0x00 r16":
            return 0, _hx(bytes(range(16)))
        if msgs == "w2@0x58 0x06 0x00 r1":
            return 0, "0x1d"
        if msgs == "w2@0x58 0x00 0x00 r64":
            return 0, _hx(self.page)
        if msgs.startswith("w66@0x58 0x00 0x00 "):
            self.page[:] = bytes(int(x, 16) for x in msgs.split()[3:])
            return 0, ""
        if msgs == "w3@0x58 0x04 0x00 0xff":
            self.lock |= 0x02
            return 0, ""
        if m := re.match(r"w\d+@0x50 (0x\w+) (0x\w+) (.*?) \|\| exit 2", msgs):   # page write + ACK poll
            off = int(m[1], 16) << 8 | int(m[2], 16)
            data = bytes(int(x, 16) for x in m[3].split())
            self.array[off:off + len(data)] = data
            return 0, ""
        if m := re.match(r"w2@0x50 (0x\w+) (0x\w+) r(\d+)$", msgs):
            off = int(m[1], 16) << 8 | int(m[2], 16)
            return 0, _hx(self.array[off:off + int(m[3])])
        return 1, ""


def _statuses(results):
    return {r.name: r.status for r in results}


# --- plans -------------------------------------------------------------------------

def test_blank_board_full_plan(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), gd32_fw=_gd32_fw(tmp_path),
               expected_registers={"devices": {}})
    res = steps.run_steps(ctx)
    st = _statuses(res)
    assert [r.name for r in res] == steps.STEP_NAMES
    assert "failed" not in st.values(), [(r.name, r.detail) for r in res if r.status == "failed"]
    assert st["preflight"] == "done" and st["hil_smoke"] == "skipped"
    for name in ("dsw1_scif", "bootstrap", "boot_sd_linux", "write_xspi", "eeprom_manifest",
                 "gd32_flash", "secure_page", "cold_boot_test", "record"):
        assert st[name] == "planned", name
    log = "\n".join(ctx.plan_log)
    assert "WOULD: EM_W area 1 sector 0x1: bl2_mmc" in log
    assert "WOULD: EM_W area 1 sector 0x300: fip" in log
    assert "WOULD: EM_SECSD EXT_CSD[177] = 0x02" in log and "EXT_CSD[179] = 0x08" in log
    assert "UNCHECKED" in log                      # eeprom preconditions without Linux
    assert not ctx.state_path.exists()             # dry run writes no state
    assert not (ctx.unit_dir / f"{SERIAL}.manifest.staged.bin").exists()
    assert ctx.bench.power.events == []
    assert {c[0] for c in ctx.bench.probe.calls} == {"savebin", "dp_id"}   # read-only probe use


def test_offline_plan_without_bench(tmp_path):
    res = steps.run_steps(_ctx(tmp_path))
    st = _statuses(res)
    assert st["preflight"] == "done" and st["bootstrap"] == "planned"
    assert "failed" not in st.values()


def test_board1_plan_refuses_eeprom_manifest(tmp_path):
    board = Board(xspi_bl2=BL2, xspi_fip=OLD_FIP)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, gd32_fw=_gd32_fw(tmp_path))
    res = steps.run_steps(ctx)
    st = _statuses(res)
    for name in ("dsw1_scif", "bootstrap", "dsw1_emmc_insert_sd", "boot_sd_linux"):
        assert st[name] == "skipped", name        # Linux already answers
    assert st["write_xspi"] == "planned"          # mtd1 holds the old FIP
    assert st["census"] == "done"
    bad = _failed(res)
    assert bad.name == "eeprom_manifest"
    assert "lacks 'ALP: DEEPX rail 0.75V up (PG)'" in bad.detail
    assert res[-1].name == "record"                # Record still runs after the refusal
    # nothing was written: no erase, no dd, no 0x50 page write, no staging
    assert not any(re.search(r"flash_erase|mtd_debug write|\bdd\b[^|]*\bof=|w\d+@0x50 .* 0x\w+ 0x\w+ 0x", c)
                   for c in board.commands)
    assert not (ctx.unit_dir / f"{SERIAL}.manifest.staged.bin").exists()


def test_no_command_ever_writes_selector_0x06(tmp_path):
    board = Board(xspi_bl2=BL2, xspi_fip=FIP)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, gd32_fw=_gd32_fw(tmp_path),
               expected_registers={"devices": {}})
    steps.run_steps(ctx)
    ident = [c for c in board.commands if "@0x58" in c]
    assert "i2ctransfer -y 0 w2@0x58 0x06 0x00 r1" in ident
    for c in ident:
        assert not re.search(r"w\d+@0x58 0x06(?! 0x00 r1$)", c), c


# --- idempotence / done semantics ------------------------------------------------------

def test_provisioned_unit_skips_writes(tmp_path):
    boot1 = bytearray(b"\x00" * (0x300 * 512 + len(FIP)))
    boot1[512:512 + len(BL2)] = BL2
    boot1[0x300 * 512:] = FIP
    bdir, b = _bundle(tmp_path)
    wic = gzip.decompress((bdir / "artifacts" / "img.wic.gz").read_bytes())
    board = Board(xspi_bl2=BL2, xspi_fip=FIP, boot1=bytes(boot1), emmc=wic)
    board.ext = {177: 0x02, 179: 0x08}
    ctx = _ctx(tmp_path, bundle=(bdir, b), bench=_bench(), linux=board, execute=True)
    res = steps.run_steps(ctx, only=["write_xspi", "write_emmc_boot", "write_rootfs"])
    assert _statuses(res) == {"preflight": "done", "write_xspi": "skipped",
                              "write_emmc_boot": "skipped", "write_rootfs": "skipped"}
    assert not any(re.search(r"flash_erase|mtd_debug|\bdd\b|mmc boot", c) for c in board.commands)
    state = json.loads(ctx.state_path.read_text(encoding="utf-8"))
    assert state["steps"]["write_xspi"]["status"] == "skipped"
    assert state["serial"] == SERIAL and state["schema"] == 1


class WritableBoard(Board):
    def put(self, local, remote):
        self.files[remote] = Path(local).read_bytes()

    def _answer(self, cmd):
        if m := re.match(r"mtd_debug write (/dev/mtd\d) 0 (\d+) (\S+)", cmd):
            data = self.files[m[3]]
            self.files[m[1]] = data + self.files[m[1]][len(data):]
            return 0, ""
        if re.match(r"flash_erase|rm -f", cmd):
            return 0, ""
        if cmd.startswith("tail -c"):
            return 0, ""   # md5 is served by FakeLinux.md5 from files
        return super()._answer(cmd)

    def md5(self, path, offset=0, size=None):
        return super().md5(path, offset, size)


def test_write_xspi_done_only_after_reprobe(tmp_path):
    board = WritableBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD_FIP)
    lt_md5 = lt.LinuxTarget.md5
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    res = steps.run_steps(ctx, only=["write_xspi"])
    assert lt_md5 is lt.LinuxTarget.md5
    wx = res[-1]
    assert wx.status == "done", wx.detail
    assert wx.evidence["xspi_fip_md5"] == hashlib.md5(FIP).hexdigest()
    assert any(c.startswith("flash_erase /dev/mtd1 0 1") for c in board.commands)


def test_state_done_with_unsatisfied_probe_reruns(tmp_path):
    board = WritableBoard(xspi_bl2=BL2, xspi_fip=OLD_FIP)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    ctx.state = {"steps": {"write_xspi": {"status": "done"}}}
    res = steps.run_steps(ctx, only=["write_xspi"])
    assert res[-1].status == "done"
    assert any("recorded done but probe says" in line for line in ctx.plan_log)


# --- refusals ------------------------------------------------------------------------------

def test_refuses_sku_mismatch(tmp_path):
    ctx = _ctx(tmp_path, sku="E1M-V2M101", preset_sku="E1M-V2M101")
    res = steps.run_steps(ctx)
    assert _failed(res).name == "preflight" and "SKU mismatch" in _failed(res).detail


def test_refuses_tier_and_override_is_recorded(tmp_path):
    d16 = b"\x00" * 64 + bytes.fromhex(MARKERS["markers"][1]["hex"])
    res = steps.run_steps(_ctx(tmp_path / "a", bundle=_bundle(tmp_path / "a", bl2=d16)))
    assert "DDR tier mismatch" in _failed(res).detail
    ctx = _ctx(tmp_path / "b", bundle=_bundle(tmp_path / "b", bl2=d16), allow_tier_mismatch="bench fix")
    res = steps.run_steps(ctx, only=["preflight"])
    assert res[0].status == "done"
    assert ctx.state["overrides"][0]["gate"] == "tier_triangle"
    assert ctx.facts["dram_tier_check"].startswith("overridden")


def test_uboot_dram_banner_is_bucketed(tmp_path):
    ctx = _ctx(tmp_path)
    ok, _ = steps.tier_gate(ctx, 3994)            # "DRAM:  3.9 GiB" on a 4 GiB unit
    assert ok.ok, ok.detail
    bad, _ = steps.tier_gate(ctx, 1946)            # "1.9 GiB"
    assert not bad.ok


def test_refuses_fip_without_rail_string(tmp_path):
    res = steps.run_steps(_ctx(tmp_path, bundle=_bundle(tmp_path, fip=OLD_FIP)))
    assert "fip_rail" in _failed(res).detail


def test_refuses_gd32_wrong_debug_port(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(probe=FakeProbe(dp_id_value=0x6BA02477)),
               linux=Board(), gd32_fw=_gd32_fw(tmp_path))
    res = steps.run_steps(ctx, only=["gd32_flash"])
    assert res[-1].status == "failed" and "wrong target" in res[-1].detail


def _lock_ready(tmp_path, board, disposition="ship", done=True):
    ctx = _ctx(tmp_path, bench=_bench(operator=FakeOperator([SERIAL])), linux=board, execute=True)
    ctx.unit_dir.mkdir(parents=True, exist_ok=True)
    (ctx.unit_dir / f"{SERIAL}.manifest.bin").write_bytes(bytes(board.array))
    (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").write_bytes(bytes(board.page))
    (ctx.unit_dir / f"{SERIAL}.unit.yaml").write_text(
        f"eeprom_unique_id: 00 11\ndisposition: {disposition}\n", encoding="utf-8")
    if done:
        ctx.state = {"steps": {s: {"status": "done"} for s in ("secure_page", "cold_boot_test")}}
        ctx.state["steps"]["cold_boot_test"]["evidence"] = {"cold_boots_passed": "3/3"}
    return ctx


def test_lock_preconditions(tmp_path):
    ctx = _lock_ready(tmp_path / "a", Board(), disposition="bench-only", done=False)
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "failed"
    for why in ("secure_page not done", "cold_boot_test not done", "disposition is bench-only"):
        assert why in r.detail
    board = Board()
    ctx = _lock_ready(tmp_path / "b", board)
    ctx.bench.operator.answers = ["2026W38-0002"]
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "failed" and "not '2026W38-0001'" in r.detail
    assert not any("0xff" in c and "@0x58 0x04" in c for c in board.commands)


def test_lock_success_and_dry_run_plan_only(tmp_path):
    board = Board()
    ctx = _lock_ready(tmp_path / "dry", board)
    ctx.execute = False
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "planned" and board.lock == 0xFD
    ctx = _lock_ready(tmp_path / "x", board)
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "done", r.detail
    assert board.lock & 0x02 and r.evidence["secure_page_state"] == "locked"


# --- ACT88760 GPIO4 early-OTP workaround ----------------------------------------------------------

def test_gd32_flash_applies_the_early_otp_workaround(tmp_path):
    """An 0x88-OTP unit: gd32_flash applies the volatile release and records
    how; this is no longer, by itself, a ship-blocking or bench-only fact."""
    fw = _gd32_fw(tmp_path)
    board = Board(act_0x10=0x88)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, gd32_fw=fw, execute=True)
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    unit.parent.mkdir(parents=True)
    unit.write_text("# hand-opened\nnotes: see repo#1234\n", encoding="utf-8")
    res = steps.run_steps(ctx, only=["gd32_flash", "record"])
    st = _statuses(res)
    assert st["gd32_flash"] == "done", res[1].detail
    assert board.regs[(8, 0x25, 0x10)] == 0x08                     # volatile workaround applied
    assert "i2cset -y 8 0x25 0x10 0x08" in board.commands
    text = unit.read_text(encoding="utf-8")
    assert "act88760_gpio4_otp: 0x88" in text
    assert "act88760_gpio4_workaround: provision (volatile 0x08)" in text
    assert "disposition: bench-only" not in text                     # no longer auto-defaulted for this
    assert "notes: see repo#1234" in text                            # manual key, inline '#', untouched
    # still blocked (disposition unset, eeprom_unique_id missing), just not because of GPIO4
    assert "blocked" in res[-1].detail
    assert "gpio4" not in res[-1].detail.lower()
    # verify used fresh probe sessions (savebin after the loadbins)
    kinds = [c[0] for c in ctx.bench.probe.calls]
    last_load = max(i for i, k in enumerate(kinds) if k == "loadbin")
    assert kinds[last_load + 1:kinds.index("reset_run")].count("savebin") == len(steps.GD32_IMAGES)


def test_gd32_flash_workaround_does_not_touch_an_operator_disposition(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(act_0x10=0x88), gd32_fw=_gd32_fw(tmp_path), execute=True)
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    unit.parent.mkdir(parents=True)
    unit.write_text("disposition: hold\n", encoding="utf-8")
    res = steps.run_steps(ctx, only=["gd32_flash", "record"])
    assert "disposition: hold" in unit.read_text(encoding="utf-8")
    assert "blocked" in res[-1].detail


def test_select_steps_rules():
    names = [s.name for s in steps.select_steps(only=["census"])]
    assert names == ["preflight", "census"]
    names = [s.name for s in steps.select_steps(start="secure_page", skip=["hil_smoke"])]
    assert names == ["preflight", "secure_page", "dsw1_xspi_remove_sd", "cold_boot_test",
                     "clkgen_verify", "record"]
    with pytest.raises(ValueError):
        steps.select_steps(only=["nope"])


def test_mfg_date_is_monday_of_serial_week(tmp_path):
    ctx = _ctx(tmp_path)
    assert ctx.mfg_date.isoformat() == "2026-09-14"


def _login_console():
    """Console for one cold cycle to a login, then console_login + discover_host."""
    return FakeConsole([(r"^\r$", "\nroot@e1m:~# "),
                        (r"^ip -4 -o addr", "2: eth0    inet 10.0.0.2/24 brd 10.0.0.255 scope global eth0\n")])


def test_eeprom_manifest_and_secure_page_execute(tmp_path):
    board = Board(xspi_bl2=BL2, xspi_fip=FIP)
    board.host = "10.0.0.2"
    console = _login_console()
    bench = _bench(console=console)
    bench.power.on_hook = lambda: console.feed("NOTICE:  BL2: v2.10\nDRAM:  3.9 GiB\n\ne1m login: ")
    ctx = _ctx(tmp_path, bench=bench, linux=board, execute=True)
    res = steps.run_steps(ctx, only=["eeprom_manifest", "secure_page"])
    st = _statuses(res)
    assert st == {"preflight": "done", "eeprom_manifest": "done", "secure_page": "done"}, \
        [(r.name, r.detail) for r in res]
    assert bench.power.events == ["off", "on"]                       # the cold cycle before re-read
    written = ctx.unit_dir / f"{SERIAL}.manifest.bin"
    assert written.read_bytes() == bytes(board.array)
    assert not (ctx.unit_dir / f"{SERIAL}.manifest.staged.bin").exists()
    assert written.read_bytes()[24:34] == SKU.encode()
    assert ctx.facts["manifest_crc32"] == ctx.facts["manifest_staged_crc32"]
    assert ctx.facts["secure_page_state"] == "written-verified"
    assert (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").read_bytes() == bytes(board.page)
    assert board.lock == 0xFD                                        # secure_page never locks
    page_writes = [c for c in board.commands if "w66@0x58" in c]
    assert len(page_writes) == 1 and page_writes[0].startswith("i2ctransfer -y 0 w66@0x58 0x00 0x00 ")


def test_eeprom_manifest_refuses_locked_or_foreign_array(tmp_path):
    board = Board(xspi_bl2=BL2, xspi_fip=FIP, lock=0xFF)
    board.array[:] = b"\x00" * 128
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    r = steps.run_steps(ctx, only=["eeprom_manifest"])[-1]
    assert r.status == "failed"
    assert "locked" in r.detail and "not blank" in r.detail
    assert not any("@0x50" in c and "|| exit 2" in c for c in board.commands)


# --- review fixes: guards, sticky facts, state invalidation -----------------------------

def test_write_rootfs_refuses_the_running_emmc_root(tmp_path):
    board = Board(root="mmcblk0p2")
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    r = steps.run_steps(ctx, only=["write_rootfs"])
    bad = _failed(r)
    assert bad.name == "write_rootfs" and "running root" in bad.detail
    assert not any(re.search(r"\bdd\b", c) for c in board.commands)


def test_new_bundle_supersedes_recorded_steps(tmp_path):
    ctx = _ctx(tmp_path)
    ctx.state = {"bundle_sha256": "old", "tool_rev": steps.tool_rev(),
                 "steps": {"cold_boot_test": {"status": "done"}}}
    steps.init_state(ctx, "new")
    assert not ctx.state_done("cold_boot_test")
    assert ctx.state["superseded"][0]["bundle_sha256"] == "old"
    steps.init_state(ctx, "new")                   # same bundle + tool: nothing superseded again
    assert len(ctx.state["superseded"]) == 1


def test_legacy_defect_key_is_informational_not_sticky_and_not_blocking(tmp_path):
    """A unit.yaml carrying the pre-decision `act88760_gpio4_defect: yes` key
    (e.g. E1M-V2M103 2026W38-0001) is never overwritten -- the tool no longer
    writes that key -- but it also no longer forces bench-only or blocks the
    ship check by itself (see test_ship_check in test_provision_ledger_out.py
    for the ship-check side)."""
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(act_0x10=0x08), execute=True)
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    unit.parent.mkdir(parents=True)
    unit.write_text("act88760_gpio4_defect: yes\n", encoding="utf-8")
    steps.run_steps(ctx, only=["record"])
    text = unit.read_text(encoding="utf-8")
    assert "act88760_gpio4_defect: yes" in text          # left alone, not in the auto set any more
    assert "disposition: bench-only" not in text          # no longer auto-defaulted from the legacy key


def test_census_records_otp_and_workaround_none_when_already_released(tmp_path):
    ctx = _ctx(tmp_path / "c", bench=_bench(), linux=Board(act_0x10=0x08))
    res = steps.run_steps(ctx, only=["census"])
    assert res[-1].evidence["act88760_gpio4_otp"] == "0x08"
    assert res[-1].evidence["act88760_gpio4_workaround"] == "none"
    assert "act88760_gpio4_defect" not in res[-1].evidence


def test_census_records_otp_default_without_a_workaround_verdict(tmp_path):
    ctx = _ctx(tmp_path / "c", bench=_bench(), linux=Board(act_0x10=0x88))
    res = steps.run_steps(ctx, only=["census"])
    assert res[-1].evidence["act88760_gpio4_otp"] == "0x88"
    assert "act88760_gpio4_workaround" not in res[-1].evidence   # decided later, by cold_boot_test


def test_cold_boot_early_otp_unit_not_released_exempts_the_gd32(tmp_path):
    board = Board(act_0x10=0x88)
    board.host = "10.0.0.2"
    board._answer_orig = board._answer
    every_but_gd32 = "".join(f"{r:x}0: " + " ".join("--" if r * 16 + c == 0x70 else f"{r * 16 + c:02x}"
                                                    for c in range(16)) + "\n" for r in range(8))
    board._answer = lambda cmd: (0, every_but_gd32) if cmd.startswith("i2cdetect") else board._answer_orig(cmd)
    console = _login_console()
    bench = _bench(console=console)
    bench.power.on_hook = lambda: console.feed(
        "NOTICE:  BL2: v2.10\nNOTICE:  BL2: SYS_LSI_MODE: 0X3c06\nDRAM:  3.9 GiB\n"
        f"{gates.RAIL_PG}\n\ne1m login: ")
    ctx = _ctx(tmp_path, bench=bench, linux=board, execute=True, cold_cycles=1)
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    cb = res[-1]
    assert cb.name == "cold_boot_test" and "0x70" not in cb.detail, cb.detail
    assert "0x70 not required" in cb.evidence.get("cold_boot_note", ""), cb.evidence
    assert cb.evidence.get("act88760_gpio4_after_boot") == "0x88"
    # the DRAM banner is recorded here too, for units that never run boot_sd_linux
    assert cb.evidence.get("dram_size_mib"), cb.evidence
    assert cb.evidence.get("uboot_dram_banner", "").startswith("DRAM:"), cb.evidence


def _everyone_acks():
    """i2cdetect output where every address 0x00..0x7f answers, including
    0x70 (the GD32): used to test the "GD32 required" path without an
    unrelated on-module device (e.g. the RTC, the PMIC) reporting missing."""
    return "".join(f"{r:x}0: " + " ".join(f"{r * 16 + c:02x}" for c in range(16)) + "\n" for r in range(8))


def test_cold_boot_released_unit_requires_the_gd32_and_records_after_boot(tmp_path):
    """reg 0x10 == 0x08 after a plain cold boot: the GD32 is required like
    any other on-module device (no exemption note), and the release is
    attributed to U-Boot for an early-OTP unit (act88760_gpio4_otp was seen
    at 0x88 earlier in this run)."""
    board = Board(act_0x10=0x08)
    board.host = "10.0.0.2"
    board._answer_orig = board._answer
    board._answer = lambda cmd: (0, _everyone_acks()) if cmd.startswith("i2cdetect") else board._answer_orig(cmd)
    console = _login_console()
    bench = _bench(console=console)
    bench.power.on_hook = lambda: console.feed(
        "NOTICE:  BL2: v2.10\nNOTICE:  BL2: SYS_LSI_MODE: 0X3c06\nDRAM:  3.9 GiB\n"
        f"{gates.RAIL_PG}\n\ne1m login: ")
    ctx = _ctx(tmp_path, bench=bench, linux=board, execute=True, cold_cycles=1)
    ctx.facts["act88760_gpio4_otp"] = "0x88"    # this run's census/gd32_flash saw the early-OTP default
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    cb = res[-1]
    assert cb.status == "done", cb.detail
    assert "0x70" not in cb.detail, cb.detail                       # no exemption note; GD32 was required
    assert cb.evidence.get("act88760_gpio4_after_boot") == "0x08"
    assert cb.evidence.get("act88760_gpio4_workaround") == "u-boot"


def test_cold_boot_fixed_otp_unit_records_workaround_none(tmp_path):
    board = Board(act_0x10=0x08)
    board.host = "10.0.0.2"
    board._answer_orig = board._answer
    board._answer = lambda cmd: (0, _everyone_acks()) if cmd.startswith("i2cdetect") else board._answer_orig(cmd)
    console = _login_console()
    bench = _bench(console=console)
    bench.power.on_hook = lambda: console.feed(
        "NOTICE:  BL2: v2.10\nNOTICE:  BL2: SYS_LSI_MODE: 0X3c06\nDRAM:  3.9 GiB\n"
        f"{gates.RAIL_PG}\n\ne1m login: ")
    ctx = _ctx(tmp_path, bench=bench, linux=board, execute=True, cold_cycles=1)
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    cb = res[-1]
    assert cb.status == "done", cb.detail
    assert cb.evidence.get("act88760_gpio4_after_boot") == "0x08"
    assert cb.evidence.get("act88760_gpio4_workaround") == "none"


def test_cold_boot_reads_the_uboot_gd32_nrst_line(tmp_path):
    """U-Boot 0011 prints what it did to reg 0x10; that line, not a guess,
    decides between an early-OTP unit (u-boot) and a fixed one (none)."""
    for line, otp, how in (
            ("ALP: ACT88760 GD32_NRST released (0x10: 0x88 -> 0x08)", "0x88", "u-boot"),
            ("ALP: ACT88760 GD32_NRST already released (0x10=0x08)", "0x08", "none")):
        board = Board(act_0x10=0x08)
        board.host = "10.0.0.2"
        board._answer_orig = board._answer
        board._answer = lambda cmd, b=board: (0, _everyone_acks()) if cmd.startswith("i2cdetect") else b._answer_orig(cmd)
        console = _login_console()
        bench = _bench(console=console)
        bench.power.on_hook = lambda c=console, ln=line: c.feed(
            "NOTICE:  BL2: v2.10\nNOTICE:  BL2: SYS_LSI_MODE: 0X3c06\nDRAM:  3.9 GiB\n"
            f"{ln}\n{gates.RAIL_PG}\n\ne1m login: ")
        ctx = _ctx(tmp_path / how, bench=bench, linux=board, execute=True, cold_cycles=1)
        res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
        cb = res[-1]
        assert cb.status == "done", cb.detail
        assert cb.evidence.get("act88760_gpio4_otp") == otp, cb.evidence
        assert cb.evidence.get("act88760_gpio4_workaround") == how, cb.evidence


def test_build_dir_unit_defaults_to_bench_only(tmp_path):
    ctx = _ctx(tmp_path, execute=True)
    ctx.bundle["release_version"] = "build-dir:deploy"
    cat = dict(CATALOGUE["keys"], rootfs_bundle_version={"group": "firmware", "source": "", "mode": "auto",
                                                        "ship_required": False})
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": cat}), encoding="utf-8")
    ctx.state = {"steps": {n: {"status": "done"} for n in steps.FLASH_STEPS}}
    res = steps.run_steps(ctx, only=["record"])
    text = (ctx.unit_dir / f"{SERIAL}.unit.yaml").read_text(encoding="utf-8")
    assert "disposition: bench-only" in text and "--build-dir" in res[-1].detail


def test_record_keeps_clkgen_and_dxm1_keys_when_the_catalogue_lists_them(tmp_path):
    """Record only merges a fact whose key is in the catalogue; the clkgen
    raw-image/i2c-addr and dxm1 md5/version keys must be present there (the
    private v2n.keys.yaml catalogue -- see docs/provisioning-v2n.md)."""
    ctx = _ctx(tmp_path, execute=True)
    new_keys = {
        "clkgen_otp_raw": {"group": "clocks_rtc", "source": "", "mode": "auto", "ship_required": False},
        "clkgen_i2c_addr": {"group": "clocks_rtc", "source": "", "mode": "auto", "ship_required": False},
        "dxm1_fw_uart_boot_md5": {"group": "firmware", "source": "", "mode": "auto", "ship_required": False},
        "dxm1_fw_md5": {"group": "firmware", "source": "", "mode": "auto", "ship_required": False},
        "dxm1_fw_version": {"group": "firmware", "source": "", "mode": "auto", "ship_required": False},
        "dxm1_uart_boot_tool_md5": {"group": "firmware", "source": "", "mode": "auto", "ship_required": False},
    }
    cat = dict(CATALOGUE["keys"], **new_keys)
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": cat}), encoding="utf-8")
    ctx.facts.update(clkgen_otp_raw="aa bb cc", clkgen_i2c_addr="0x69",
                     dxm1_fw_uart_boot_md5=lt.DXM1_FW_UART_BOOT_MD5, dxm1_fw_md5=lt.DXM1_FW_MD5,
                     dxm1_fw_version=lt.DXM1_FW_VERSION, dxm1_uart_boot_tool_md5=lt.DXM1_UART_BOOT_TOOL_MD5)
    steps.run_steps(ctx, only=["record"])
    text = (ctx.unit_dir / f"{SERIAL}.unit.yaml").read_text(encoding="utf-8")
    for key in new_keys:
        assert f"{key}:" in text, text


def test_override_reaches_the_ledger_fact(tmp_path):
    ctx = _ctx(tmp_path)
    steps._record_override(ctx, "tier_triangle", "bench fix")
    assert ctx.facts["provision_overrides"] == "tier_triangle: bench fix"


def test_lock_refused_when_secure_page_differs_from_staged(tmp_path):
    board = Board()
    ctx = _lock_ready(tmp_path, board)
    (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").write_bytes(b"\x00" * 64)
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "failed" and "secure page differs" in r.detail
    assert board.lock == 0xFD


def test_gd32_probe_reads_nothing_from_a_wrong_debug_port(tmp_path):
    probe = FakeProbe(dp_id_value=0x6BA02477)
    ctx = _ctx(tmp_path, bench=_bench(probe=probe), gd32_fw=_gd32_fw(tmp_path))
    assert isinstance(steps.Gd32Flash().probe(ctx), steps.Unknown)
    assert [c[0] for c in probe.calls] == ["dp_id"]


# --- clkgen_verify ---------------------------------------------------------------------

def test_clkgen_verify_pass(tmp_path):
    board = Board()
    image = bytearray(lt.CLKGEN_OTP_IMAGE)
    image[0x21], image[0x24] = 0xC0, 0x8E
    for reg, val in enumerate(image):
        board.regs[(8, 0x69, reg)] = val
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    ctx.boot_text = "NOTICE:  BL2: v2.10\nALP: 5L35023B clock: success\n\ne1m login: "
    res = steps.run_steps(ctx, only=["clkgen_verify"])
    r = res[-1]
    assert r.name == "clkgen_verify" and r.status == "done", r.detail
    assert r.evidence["clkgen_i2c_addr"] == "0x69"
    assert r.evidence["clkgen_otp_raw"] == " ".join(f"{b:02x}" for b in image)


def test_clkgen_verify_fails_on_mismatch_and_missing_boot_line(tmp_path):
    board = Board()
    for reg, val in enumerate(lt.CLKGEN_OTP_IMAGE):   # factory OTP: fixup not applied
        board.regs[(8, 0x69, reg)] = val
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)
    ctx.boot_text = "no clkgen line here\n"
    res = steps.run_steps(ctx, only=["clkgen_verify"])
    r = res[-1]
    assert r.status == "failed"
    assert "reg 0x21" in r.detail and "5L35023B clock" in r.detail
    assert "#2293" in r.detail and "patch 0007" in r.detail


# --- dxm1_npu_flash (BENCH-PENDING) ------------------------------------------------------

def test_dxm1_npu_flash_skipped_for_v2n(tmp_path):
    bdir, b = _bundle(tmp_path, family="v2n")
    ctx = _ctx(tmp_path, bundle=(bdir, b), bench=_bench(), dxm1_flash=True)
    r = steps.Dxm1NpuFlash().run(ctx)
    assert r.status == "skipped" and "not a V2M" in r.detail


def test_dxm1_npu_flash_skipped_by_default(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench())  # bundle family defaults to v2n-m1
    r = steps.Dxm1NpuFlash().run(ctx)
    assert r.status == "skipped" and "BENCH-PENDING" in r.detail


def test_dxm1_npu_flash_refuses_tbd_bench_key(tmp_path):
    bench = _bench()
    bench.raw["dxm1"] = {"gpio_chip": "chip0", "uart_mux_line": 5, "reset_line": 6,
                         "uart_device": "/dev/ttySC1", "uart_boot": "uart_boot",
                         "fw_uart_boot": "fw_uart_boot.bin", "fw": None}
    ctx = _ctx(tmp_path, bench=bench, dxm1_flash=True, execute=True)
    with pytest.raises(steps.Refused, match="dxm1.fw is TBD"):
        steps.Dxm1NpuFlash().run(ctx)


def test_dxm1_npu_flash_refuses_wrong_firmware_md5(tmp_path):
    fw_dir = tmp_path / "dxm1fw"
    fw_dir.mkdir()
    (fw_dir / "fw_uart_boot.bin").write_bytes(b"not the real bootloader")
    (fw_dir / "fw.bin").write_bytes(b"not the real firmware")
    bench = _bench()
    bench.raw["dxm1"] = {"gpio_chip": "chip0", "uart_mux_line": 5, "reset_line": 6,
                         "uart_device": "/dev/ttySC1", "uart_boot": str(fw_dir / "uart_boot"),
                         "fw_uart_boot": str(fw_dir / "fw_uart_boot.bin"),
                         "fw": str(fw_dir / "fw.bin")}
    ctx = _ctx(tmp_path, bench=bench, dxm1_flash=True, execute=True)
    with pytest.raises(steps.Refused, match="md5"):
        steps.Dxm1NpuFlash().run(ctx)


@pytest.mark.parametrize("line_key,value,match", [
    ("uart_mux_line", "5", "must be an int"),
    ("uart_mux_line", 52, "P64"),
    ("reset_line", 53, "P65"),
])
def test_dxm1_npu_flash_refuses_bad_gpio_lines(tmp_path, line_key, value, match):
    bench = _bench()
    bench.raw["dxm1"] = {"gpio_chip": "chip0", "uart_mux_line": 61, "reset_line": 86,
                         "uart_device": "/dev/ttySC1", "uart_boot": "uart_boot",
                         "fw_uart_boot": "fw_uart_boot.bin", "fw": "fw.bin"}
    bench.raw["dxm1"][line_key] = value
    ctx = _ctx(tmp_path, bench=bench, dxm1_flash=True, execute=True)
    with pytest.raises(steps.Refused, match=match):
        steps.Dxm1NpuFlash().run(ctx)


def test_dxm1_npu_flash_refuses_equal_mux_and_reset_lines(tmp_path):
    bench = _bench()
    bench.raw["dxm1"] = {"gpio_chip": "chip0", "uart_mux_line": 61, "reset_line": 61,
                         "uart_device": "/dev/ttySC1", "uart_boot": "uart_boot",
                         "fw_uart_boot": "fw_uart_boot.bin", "fw": "fw.bin"}
    ctx = _ctx(tmp_path, bench=bench, dxm1_flash=True, execute=True)
    with pytest.raises(steps.Refused, match="uart_mux_line == dxm1.reset_line"):
        steps.Dxm1NpuFlash().run(ctx)


def test_dxm1_npu_flash_refuses_wrong_uart_boot_tool_md5(tmp_path):
    fw_dir = tmp_path / "dxm1fw"
    fw_dir.mkdir()
    (fw_dir / "fw_uart_boot.bin").write_bytes(bytes.fromhex("00" * 4))
    (fw_dir / "fw.bin").write_bytes(bytes.fromhex("00" * 4))
    (fw_dir / "uart_boot").write_bytes(b"not the vendor tool")
    # patch the pinned firmware md5s to match our placeholder files so only
    # the uart_boot tool's md5 mismatch is exercised
    want_boot = hashlib.md5((fw_dir / "fw_uart_boot.bin").read_bytes()).hexdigest()
    want_fw = hashlib.md5((fw_dir / "fw.bin").read_bytes()).hexdigest()
    orig_boot, orig_fw = lt.DXM1_FW_UART_BOOT_MD5, lt.DXM1_FW_MD5
    lt.DXM1_FW_UART_BOOT_MD5, lt.DXM1_FW_MD5 = want_boot, want_fw
    try:
        bench = _bench()
        bench.raw["dxm1"] = {"gpio_chip": "chip0", "uart_mux_line": 61, "reset_line": 86,
                             "uart_device": "/dev/ttySC1", "uart_boot": str(fw_dir / "uart_boot"),
                             "fw_uart_boot": str(fw_dir / "fw_uart_boot.bin"),
                             "fw": str(fw_dir / "fw.bin")}
        ctx = _ctx(tmp_path, bench=bench, dxm1_flash=True, execute=True)
        with pytest.raises(steps.Refused, match="uart_boot.*md5"):
            steps.Dxm1NpuFlash().run(ctx)
    finally:
        lt.DXM1_FW_UART_BOOT_MD5, lt.DXM1_FW_MD5 = orig_boot, orig_fw


def test_dxm1_npu_flash_execute_fails_when_pcie_endpoint_absent(tmp_path):
    """The real-firmware execute path: only the PCIe verify should fail."""
    fw_dir = tmp_path / "dxm1fw"
    fw_dir.mkdir()
    boot_bin = fw_dir / "fw_uart_boot.bin"
    fw_bin = fw_dir / "fw.bin"
    tool_bin = fw_dir / "uart_boot"
    boot_bin.write_bytes(b"boot")
    fw_bin.write_bytes(b"fw")
    tool_bin.write_bytes(b"tool")
    orig = (lt.DXM1_FW_UART_BOOT_MD5, lt.DXM1_FW_MD5, lt.DXM1_UART_BOOT_TOOL_MD5)
    lt.DXM1_FW_UART_BOOT_MD5 = hashlib.md5(boot_bin.read_bytes()).hexdigest()
    lt.DXM1_FW_MD5 = hashlib.md5(fw_bin.read_bytes()).hexdigest()
    lt.DXM1_UART_BOOT_TOOL_MD5 = hashlib.md5(tool_bin.read_bytes()).hexdigest()
    try:
        board = Board()
        board.host = "10.0.0.2"
        console = _login_console()
        bench = _bench(console=console)
        # pinctrl chip-base lookup, sysfs P75/PA6 dir resolution (already
        # named + ownership-verified, no export needed), the reset pulse,
        # two uart_boot calls, then P75 released -- all scripted
        # permissively so only the PCIe-empty cold boot causes the failure.
        console_hooks = {"n": 0}

        def on_power():
            console.feed("NOTICE:  BL2: v2.10\nDRAM:  3.9 GiB\n\ne1m login: ")
        bench.power.on_hook = on_power
        board._answer_orig = board._answer

        def answer(cmd):
            if cmd.startswith("for d in /sys/class/gpio/gpiochip*"):
                return 0, "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"
            if cmd == "cat /sys/class/gpio/gpiochip416/base":
                return 0, "416\n"
            if cmd == "test -e /sys/class/gpio/gpio477/value":
                return 1, ""
            if cmd == "test -e /sys/class/gpio/P75/value":
                return 0, ""
            if cmd == "readlink -f /sys/class/gpio/gpiochip416/device":
                return 0, "/sys/devices/platform/soc/10410000.pinctrl\n"
            if cmd == "readlink -f /sys/class/gpio/P75/device":
                return 0, "/sys/devices/platform/soc/10410000.pinctrl/gpiochip0\n"
            if cmd == "echo high > /sys/class/gpio/P75/direction":
                return 0, ""
            if cmd == "test -e /sys/class/gpio/gpio502/value":
                return 1, ""
            if cmd == "test -e /sys/class/gpio/PA6/value":
                return 0, ""
            if cmd == "readlink -f /sys/class/gpio/PA6/device":
                return 0, "/sys/devices/platform/soc/10410000.pinctrl/gpiochip0\n"
            if re.search(r"echo low > /sys/class/gpio/PA6/direction && sleep 0\.1 && "
                         r"echo high > /sys/class/gpio/PA6/direction", cmd):
                return 0, ""
            if re.search(r"uart_boot -d /dev/ttySC1 -f", cmd):
                return 0, "bootloader ok\n"
            if re.search(r"uart_boot -d /dev/ttySC1 -F", cmd):
                return 0, "app ok\n"
            if cmd == "echo low > /sys/class/gpio/P75/direction":
                return 0, ""
            if cmd == "ls /sys/bus/pci/devices":
                return 0, "0000:00:00.0\n"       # root port only: no DEEPX endpoint
            if cmd == "cat /sys/bus/pci/devices/0000:00:00.0/class":
                return 0, "0x060400\n"
            if cmd.startswith("chmod +x"):
                return 0, ""
            return board._answer_orig(cmd)
        board._answer = answer
        bench.raw["dxm1"] = {"gpio_chip": "10410000.pinctrl", "uart_mux_line": 61, "reset_line": 86,
                             "uart_device": "/dev/ttySC1", "uart_boot": str(tool_bin),
                             "fw_uart_boot": str(boot_bin), "fw": str(fw_bin)}
        ctx = _ctx(tmp_path, bench=bench, linux=board, dxm1_flash=True, execute=True)
        r = steps.Dxm1NpuFlash().run(ctx)
        raise AssertionError("expected Refused, got a result: " + repr(r))
    except steps.Refused as e:
        assert "DEEPX endpoint" in str(e)
    finally:
        lt.DXM1_FW_UART_BOOT_MD5, lt.DXM1_FW_MD5, lt.DXM1_UART_BOOT_TOOL_MD5 = orig


class _AckBoard(Board):
    """A Board whose I2C buses ACK exactly `acks` (bus -> set of addresses)."""

    def __init__(self, acks, **kw):
        super().__init__(**kw)
        self.acks = acks

    def _answer(self, cmd):
        if m := re.match(r"i2cdetect -y -r (\d+)", cmd):
            got = self.acks.get(int(m[1]), set())
            rows = []
            for base in range(0, 0x80, 0x10):
                cells = [f"{a:02x}" if a in got else "--" for a in range(base, base + 16)]
                rows.append(f"{base:02x}: " + " ".join(cells))
            return 0, "\n".join(rows) + "\n"
        return super()._answer(cmd)


def _preset_acks(ctx):
    return {bus: set(a) for bus, a in lt.expected_i2c(ctx.preset, ctx.bench.i2c_bus).items()}


def test_som_presence_passes_when_every_preset_device_acks(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench())
    ctx.linux = _AckBoard(_preset_acks(ctx))
    assert steps.som_presence_problems(ctx) == []


def test_som_presence_ignores_a_gd32_held_in_reset(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench())
    acks = _preset_acks(ctx)
    for a in acks.values():
        a.discard(steps.GD32_BRIDGE_ADDR)
    ctx.linux = _AckBoard(acks)
    assert steps.som_presence_problems(ctx) == []


def test_som_presence_flags_a_different_som(tmp_path):
    """A V2M preset on a bench whose module has no DA9292 (0x1E): refuse
    before any destructive write."""
    ctx = _ctx(tmp_path, bench=_bench())
    acks = _preset_acks(ctx)
    for a in acks.values():
        a.discard(0x1E)
    ctx.linux = _AckBoard(acks)
    probs = steps.som_presence_problems(ctx)
    assert probs and "0x1e" in probs[0]


def test_boot_sd_linux_probe_refuses_an_emmc_root_on_resume(tmp_path, monkeypatch):
    ctx = _ctx(tmp_path, bench=_bench(), transfer="sd")
    ctx.linux = Board()
    monkeypatch.setattr(ctx, "linux_up", lambda: True)
    monkeypatch.setattr(lt, "root_device", lambda t: "mmcblk0p2")
    monkeypatch.setattr(lt, "resolve_emmc", lambda t: "mmcblk0")
    assert isinstance(steps.BootSdLinux().probe(ctx), steps.Unsatisfied)


def test_linux_up_attaches_the_configured_host_when_detect_was_skipped(tmp_path, monkeypatch):
    # --only boot_sd_linux,write_rootfs on a board already up: ctx.linux is
    # still None (detect never ran), so the probe used to report Unknown and
    # cold-cycle the board. A configured linux.host must be attached instead.
    b = _bench()
    b.linux_host = "192.0.2.7"
    ctx = _ctx(tmp_path, bench=b)
    seen = []
    monkeypatch.setattr(lt.LinuxTarget, "run",
                        lambda self, cmd, **kw: seen.append(self.host) or lt.CmdResult(0, "", ""))
    assert ctx.linux is None and ctx.linux_up()
    assert seen == ["192.0.2.7"]
    assert not steps.Ctx.linux_up(_ctx(tmp_path / "b", bench=_bench()))


def test_need_linux_attaches_the_configured_host_on_a_forced_step(tmp_path):
    # --only gd32_flash --force-step gd32_flash on a board already up: the
    # step calls need_linux() without any probe having attached ctx.linux,
    # which refused with "boot_sd_linux has not run" (E1M-V2M103, 2026-09-29).
    b = _bench()
    b.linux_host = "192.0.2.7"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    assert ctx.linux is None
    assert ctx.need_linux().host == "192.0.2.7"
    with pytest.raises(steps.Refused):
        _ctx(tmp_path / "b", bench=_bench(), execute=True).need_linux()


def test_preflight_refuses_execute_without_pmic_expect(tmp_path):
    ctx = _ctx(tmp_path, execute=True, expected_registers=None)
    res = steps.Preflight().run(ctx)
    assert res.status == "failed" and "pmic_expect" in res.detail
    ctx = _ctx(tmp_path / "b", execute=False, expected_registers=None)
    assert "pmic_expect" not in steps.Preflight().run(ctx).detail


def test_detect_drops_a_stale_bootstrap_record_when_the_unit_is_silent(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(console=FakeConsole([])))
    ctx.state = {"steps": {"bootstrap": {"status": "done"}}}
    res = steps.Detect().run(ctx)
    assert "silent" in res.detail and "bootstrap dropped" in res.detail
    assert not ctx.state_done("bootstrap")


def test_detect_keeps_the_bootstrap_record_when_bl2_prints(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(console=FakeConsole([(None, "NOTICE:  BL2: v2.10.5(release):alp\n")])))
    ctx.state = {"steps": {"bootstrap": {"status": "done"}}}
    steps.Detect().run(ctx)
    assert ctx.state_done("bootstrap")


# --- fail-closed cold boots / record only after flashing (#2464, #2465) -------------------------

@pytest.mark.parametrize("n", [0, -1])
def test_cold_boot_test_with_no_cycles_fails_not_passes(tmp_path, n):
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(), execute=True, cold_cycles=n)
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    assert res[-1].status == "failed" and ">= 1" in res[-1].detail, res[-1]
    assert not ctx.state_done("cold_boot_test")


@pytest.mark.parametrize("evidence", [{}, {"cold_boots_passed": "0/0"}, {"cold_boots_passed": "1/3"}])
def test_lock_refused_without_recorded_clean_cold_boots(tmp_path, evidence):
    board = Board()
    ctx = _lock_ready(tmp_path, board)
    ctx.state["steps"]["cold_boot_test"]["evidence"] = evidence     # state says done, evidence says no boots
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "failed" and "no clean cold boots" in r.detail, r.detail
    assert board.lock == 0xFD


def test_lock_refused_when_any_step_failed_in_the_state(tmp_path):
    board = Board()
    ctx = _lock_ready(tmp_path, board)
    ctx.state["steps"]["write_rootfs"] = {"status": "failed"}
    r = steps.run_steps(ctx, steps=[steps.SecurePageLock])[0]
    assert r.status == "failed" and "write_rootfs failed" in r.detail, r.detail


def _record_with_bundle_facts(tmp_path, flash_status):
    ctx = _ctx(tmp_path, execute=True)
    cat = dict(CATALOGUE["keys"], **{k: {"group": "firmware", "source": "", "mode": "auto",
                                        "ship_required": False}
                                    for k in ("bl2_sha256", "rootfs_bundle_version")})
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": cat}), encoding="utf-8")
    ctx.state = {"steps": {n: {"status": flash_status} for n in steps.FLASH_STEPS}}
    ctx.facts.update(bl2_sha256="ab" * 32, rootfs_bundle_version="som-9.9.9", gd32_dp_id="0x1")
    steps.run_steps(ctx, only=["record"])
    return (ctx.unit_dir / f"{SERIAL}.unit.yaml").read_text(encoding="utf-8")


def test_record_omits_bundle_facts_when_flashing_did_not_succeed(tmp_path):
    text = _record_with_bundle_facts(tmp_path, "failed")
    assert "bl2_sha256" not in text and "rootfs_bundle_version" not in text, text
    assert "gd32_dp_id" in text          # facts observed on the unit still land


def test_record_writes_bundle_facts_after_successful_flashing(tmp_path):
    text = _record_with_bundle_facts(tmp_path, "done")
    assert "bl2_sha256" in text and "rootfs_bundle_version: som-9.9.9" in text, text


def _record_after_supersession(tmp_path, old_sha, version="som-9.9.9", groups=None, cur_steps=None):
    ctx = _ctx(tmp_path, execute=True)
    cat = dict(CATALOGUE["keys"], **{k: {"group": "firmware", "source": "", "mode": "auto",
                                        "ship_required": False}
                                    for k in ("bl2_sha256", "rootfs_bundle_version")})
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": cat}), encoding="utf-8")
    ev = {"bl2_sha256": "ab" * 32, "rootfs_bundle_version": version}
    def grp(sha, status="done"):
        return {"bundle_sha256": sha, "tool_rev": "oldrev",
                "steps": {n: {"status": status, "at": "2026-09-30T20:42:45Z", "evidence": ev}
                          for n in steps.FLASH_STEPS}}
    ctx.state = {"bundle_sha256": "cur", "steps": cur_steps or {},
                 "superseded": groups(grp) if groups else [grp(old_sha)]}
    ctx.bundle["release_version"] = version
    steps.run_steps(ctx, only=["record"])
    u = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    return u.read_text(encoding="utf-8") if u.exists() else ""


def test_record_takes_bundle_facts_from_a_tool_rev_only_supersession(tmp_path):
    text = _record_after_supersession(tmp_path, "cur")
    assert "rootfs_bundle_version: som-9.9.9" in text and "bl2_sha256" in text, text


def _no_bundle_facts(text):
    assert "rootfs_bundle_version" not in text and "bl2_sha256" not in text, text


def test_record_ignores_flash_steps_superseded_by_a_different_bundle(tmp_path):
    _no_bundle_facts(_record_after_supersession(tmp_path, "other"))


def test_record_newer_different_bundle_hides_an_older_same_bundle_flash(tmp_path):
    # Y (other bundle) was written AFTER X (current bundle): X's bytes are gone.
    _no_bundle_facts(_record_after_supersession(
        tmp_path, None, groups=lambda g: [g("cur"), g("other")]))


def test_record_current_failed_flash_blocks_the_superseded_fallback(tmp_path):
    _no_bundle_facts(_record_after_supersession(
        tmp_path, "cur", cur_steps={"write_xspi": {"status": "failed"}}))


def test_record_newest_superseded_same_bundle_failed_write_blocks_fallback(tmp_path):
    _no_bundle_facts(_record_after_supersession(
        tmp_path, None, groups=lambda g: [g("cur"), g("cur", "failed")]))


def test_record_superseded_build_dir_provenance_still_blocks_ship(tmp_path):
    text = _record_after_supersession(tmp_path, "cur", version="build-dir:/x")
    assert "rootfs_bundle_version: build-dir:/x" in text
    f = tmp_path / "written.unit.yaml"
    f.write_text(text, encoding="utf-8")
    unit = ledger_out.read_unit_yaml(f)
    assert unit["disposition"] == "bench-only"       # record's build-dir default
    assert any("unsigned --build-dir" in r for r in ledger_out.ship_check({**unit, "disposition": "ship"}, {}))



def test_record_merges_evidence_of_steps_done_in_earlier_runs(tmp_path):
    """--only/--from runs: a step finished earlier left its facts as state-file
    evidence; Record folds them in, and this run's facts win on a clash."""
    ctx = _ctx(tmp_path, execute=True)
    ctx.state["steps"] = {
        "gd32_flash": {"status": "done", "evidence": {"gd32_dp_id": "earlier", "act88760_gpio4_otp": "0x08"}},
        "eeprom": {"status": "failed", "evidence": {"act88760_gpio4_workaround": "from-failed"}},
    }
    ctx.facts["act88760_gpio4_otp"] = "0x88"
    steps.run_steps(ctx, only=["record"])
    text = (ctx.unit_dir / f"{SERIAL}.unit.yaml").read_text(encoding="utf-8")
    assert "gd32_dp_id: earlier" in text
    assert "act88760_gpio4_otp: 0x88" in text and "0x08" not in text
    assert "from-failed" not in text


def test_gd32_fw_version_reads_the_version_file(tmp_path):
    fw = _gd32_fw(tmp_path)
    ctx = _ctx(tmp_path, gd32_fw=fw)
    assert steps.Gd32Flash._fw_version(ctx) == {}
    (fw / "VERSION").write_text("0.2.9\n", encoding="utf-8")
    assert steps.Gd32Flash._fw_version(ctx) == {"gd32_fw_version": "0.2.9"}


# --- unit identity: the eMMC CID behind the address must be this serial's --------------------------

CID = "150100464e4d4e414d1234567890ab4f"


def _cid_ctx(tmp_path, monkeypatch, seen, recorded):
    b = _bench()
    b.linux_host = "192.0.2.7"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    if recorded:
        ctx.state = {"steps": {"bootstrap": {"status": "done", "evidence": {"emmc_cid_raw": recorded}}}}
    reads = []
    monkeypatch.setattr(lt, "read_emmc_cid", lambda t: reads.append(t.host) or seen)
    return ctx, reads


def test_need_linux_refuses_a_different_unit_behind_the_address(tmp_path, monkeypatch):
    ctx, _ = _cid_ctx(tmp_path, monkeypatch, "aa" + CID[2:], CID)
    with pytest.raises(steps.Refused) as e:
        ctx.need_linux()
    msg = str(e.value)
    assert "192.0.2.7" in msg and "aa" + CID[2:] in msg and CID in msg and SERIAL in msg


def test_need_linux_proceeds_on_a_matching_cid_and_reads_fresh_every_time(tmp_path, monkeypatch):
    # sysfs form: real last byte; bootstrap form: synthesised end byte, upper case
    ctx, reads = _cid_ctx(tmp_path, monkeypatch, CID, CID[:30].upper() + "01")
    assert ctx.need_linux().host == "192.0.2.7"
    ctx.need_linux()
    assert reads == ["192.0.2.7", "192.0.2.7"]       # no cache: two calls, two reads


def test_need_linux_proceeds_on_first_contact_without_reading_the_cid(tmp_path, monkeypatch):
    ctx, reads = _cid_ctx(tmp_path, monkeypatch, "00" * 16, None)
    assert ctx.need_linux().host == "192.0.2.7" and reads == []


def test_identity_of_the_parse_cid_output_matches_sysfs(tmp_path, monkeypatch):
    from provision import scif_writer as sw
    from tests.scripts.test_provision_scif_writer import CID_OUT

    rec = sw.parse_cid(CID_OUT)["emmc_cid_raw"]
    sysfs = rec[:30] + "00"                          # a host driver that drops CRC7 + end bit
    ctx, _ = _cid_ctx(tmp_path, monkeypatch, sysfs, rec)
    assert ctx.need_linux() is not None


def test_recorded_cid_survives_a_superseded_state_and_the_unit_yaml(tmp_path, monkeypatch):
    ctx, _ = _cid_ctx(tmp_path, monkeypatch, "aa" + CID[2:], None)
    ctx.state = {"steps": {}, "superseded": [
        {"steps": {"bootstrap": {"status": "done", "evidence": {"emmc_cid_raw": CID}}}}]}
    with pytest.raises(steps.Refused):
        ctx.need_linux()
    ctx.state = {"steps": {}}
    assert ctx.recorded_cid() == ""
    ctx.unit_dir.mkdir(parents=True, exist_ok=True)
    (ctx.unit_dir / f"{SERIAL}.unit.yaml").write_text(f"emmc_cid_raw: {CID}\n", encoding="utf-8")
    assert ctx.recorded_cid() == CID
    with pytest.raises(steps.Refused):
        ctx.need_linux()


def test_secure_page_lock_rechecks_identity_right_before_the_write(tmp_path, monkeypatch):
    board = Board()
    ctx = _lock_ready(tmp_path / "x", board)
    calls = []
    monkeypatch.setattr(type(ctx), "need_linux", lambda self: calls.append(board.lock) or self.linux)
    assert steps.run_steps(ctx, steps=[steps.SecurePageLock])[0].status == "done"
    assert calls == [0xFD]                            # checked while still unlocked, before the lock


def test_secure_page_lock_refused_when_the_unit_changed(tmp_path, monkeypatch):
    board = Board()
    ctx = _lock_ready(tmp_path / "x", board)
    monkeypatch.setattr(type(ctx), "need_linux",
                        lambda self: (_ for _ in ()).throw(steps.Refused("another unit")))
    assert steps.run_steps(ctx, steps=[steps.SecurePageLock])[0].status != "done"
    assert board.lock == 0xFD


def test_accept_cid_change_overrides_only_a_differing_recorded_cid(tmp_path, monkeypatch):
    other = "aa" + CID[2:]
    ctx, _ = _cid_ctx(tmp_path, monkeypatch, other, CID)
    ctx.accept_cid_change = "eMMC replaced"
    ctx.need_linux()
    assert [o["gate"] for o in ctx.state["overrides"]] == ["emmc_cid_change"]
    assert ctx.recorded_cid() == other
    ctx.need_linux()                                  # anchor adopted: now matches, no new override
    assert len(ctx.state["overrides"]) == 1
    first, _ = _cid_ctx(tmp_path / "f", monkeypatch, other, None)
    first.accept_cid_change = "first"
    first.need_linux()
    assert not first.state.get("overrides") and first.recorded_cid() == other


def test_detect_in_uboot_then_bootstrap_is_not_refused(tmp_path, monkeypatch):
    b = _bench(console=FakeConsole([]))
    b.linux_host = "192.0.2.7"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    monkeypatch.setattr(lt.LinuxTarget, "run", lambda self, cmd, **kw: lt.CmdResult(255, "", "no route"))
    assert "silent" in steps.Detect().run(ctx).detail
    assert ctx.mutate("anything", lambda: "ran") == "ran"
    dry = _ctx(tmp_path / "d", bench=_bench(console=FakeConsole([])))
    assert steps.Bootstrap().run(dry).status != "failed"


def test_accept_cid_change_adopts_once_then_refuses_another_swap(tmp_path, monkeypatch):
    other = "aa" + CID[2:]
    ctx, _ = _cid_ctx(tmp_path, monkeypatch, other, CID)
    ctx.accept_cid_change = "eMMC replaced"
    ctx.need_linux()                                  # adopts `other`
    third = "bb" + CID[2:]
    monkeypatch.setattr(lt, "read_emmc_cid", lambda t: third)
    with pytest.raises(steps.Refused):                # a second swap in the same run is not adopted
        ctx.need_linux()


def test_census_after_gd32_flash_keeps_the_early_otp_evidence(tmp_path):
    """gd32_flash runs before census: its volatile 0x08 write must not make census
    record OTP 0x08 / workaround none over the 0x88 / provision (volatile) it recorded."""
    board = Board(act_0x10=0x88)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, gd32_fw=_gd32_fw(tmp_path), execute=True)
    res = steps.run_steps(ctx, only=["gd32_flash", "census"])
    assert board.regs[(8, 0x25, 0x10)] == 0x08                  # census would read the released value
    assert ctx.facts["act88760_gpio4_otp"] == "0x88"
    assert ctx.facts["act88760_gpio4_workaround"] == "provision (volatile 0x08)"
    census = next(r for r in res if r.name == "census")
    assert "act88760_gpio4_otp" not in census.evidence
