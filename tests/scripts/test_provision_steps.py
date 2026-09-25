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
from provision import gates, steps
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
        "act88760_gpio4_defect": {"group": "power", "source": "0x25", "mode": "auto", "ship_required": False},
        "act88760_gpio4_workaround": {"group": "power", "source": "", "mode": "auto", "ship_required": False},
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
        if m := re.match(r"head -c (\d+) /dev/mtd1 \| grep", cmd):
            has = gates.RAIL_PG.encode() in self.files["/dev/mtd1"][: int(m[1])]
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
        if cmd.startswith("dmesg"):
            return 0, "gd32: GD32 bridge protocol v1\n"
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
    assert not any(re.search(r"flash_erase|mtd_debug write|\bdd\b|w\d+@0x50 .* 0x\w+ 0x\w+ 0x", c)
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


# --- GPIO4 defect -> bench-only ------------------------------------------------------------------

def test_gpio4_defect_unit_is_bench_only(tmp_path):
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
    assert "act88760_gpio4_defect: yes" in text
    assert "act88760_gpio4_workaround: volatile 0x08" in text
    assert "disposition: bench-only" in text
    assert "notes: see repo#1234" in text                            # manual key, inline '#', untouched
    assert "blocked" in res[-1].detail and "gpio4_defect" in res[-1].detail
    # verify used fresh probe sessions (savebin after the loadbins)
    kinds = [c[0] for c in ctx.bench.probe.calls]
    last_load = max(i for i, k in enumerate(kinds) if k == "loadbin")
    assert kinds[last_load + 1:kinds.index("reset_run")].count("savebin") == len(steps.GD32_IMAGES)


def test_defect_never_replaces_an_operator_disposition(tmp_path):
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


def test_defect_is_sticky_and_census_detects_it(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), linux=Board(act_0x10=0x08), execute=True)
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    unit.parent.mkdir(parents=True)
    unit.write_text("act88760_gpio4_defect: yes\nact88760_gpio4_workaround: volatile 0x08\n", encoding="utf-8")
    ctx.facts.update(act88760_gpio4_defect="no", act88760_gpio4_workaround="none")
    steps.run_steps(ctx, only=["record"])
    text = unit.read_text(encoding="utf-8")
    assert "act88760_gpio4_defect: yes" in text and "volatile 0x08" in text
    assert "disposition: bench-only" in text
    ctx = _ctx(tmp_path / "c", bench=_bench(), linux=Board(act_0x10=0x88))
    res = steps.run_steps(ctx, only=["census"])
    assert res[-1].evidence["act88760_gpio4_defect"] == "yes"


def test_defect_unit_cold_boot_does_not_require_the_gd32(tmp_path):
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
    ctx.facts["act88760_gpio4_defect"] = "yes"
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    cb = res[-1]
    assert cb.name == "cold_boot_test" and "0x70" not in cb.detail, cb.detail
    assert "0x70 not required" in cb.evidence.get("cold_boot_note", ""), cb.evidence


def test_build_dir_unit_defaults_to_bench_only(tmp_path):
    ctx = _ctx(tmp_path, execute=True)
    ctx.bundle["release_version"] = "build-dir:deploy"
    cat = dict(CATALOGUE["keys"], rootfs_bundle_version={"group": "firmware", "source": "", "mode": "auto",
                                                        "ship_required": False})
    (ctx.ledger_root / "schema" / "v2n.keys.yaml").write_text(
        yaml.safe_dump({"schema": 1, "family": "v2n", "keys": cat}), encoding="utf-8")
    res = steps.run_steps(ctx, only=["record"])
    text = (ctx.unit_dir / f"{SERIAL}.unit.yaml").read_text(encoding="utf-8")
    assert "disposition: bench-only" in text and "--build-dir" in res[-1].detail


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
    assert r.evidence["clkgen_dash_code"] == "0x00"
    assert r.evidence["clkgen_fixup_applied"] == "true"
    assert r.evidence["clkgen_otp_sha256"] == hashlib.sha256(lt.CLKGEN_OTP_IMAGE).hexdigest()


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
