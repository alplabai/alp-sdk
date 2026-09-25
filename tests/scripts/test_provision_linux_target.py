"""provision.linux_target: every helper driven through a scripted SSH runner
(FakeSSH replays stdout per command regex). All identity values are
synthetic; storage part identifiers are neutral."""

from __future__ import annotations

import gzip
import hashlib
import re
import subprocess
import zlib
from types import SimpleNamespace

import pytest

from provision import gates
from provision import linux_target as lt
from provision.bench import BenchError
from .provision_fakes import FakeConsole

UNIQUE_ID = "a5 5a 00 11 22 33 44 55 66 77 88 99 aa bb cc 00"   # synthetic


def _hx(data: bytes) -> str:
    return " ".join(f"0x{b:02x}" for b in data)


def _i2cdetect(addrs: set[int]) -> str:
    rows = ["     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f"]
    for base in range(0, 0x80, 0x10):
        cells = []
        for a in range(base, base + 0x10):
            if a < 0x03 or a > 0x77:
                cells.append("  ")
            elif a in addrs:
                cells.append("UU" if a == 0x52 else f"{a:02x}")
            else:
                cells.append("--")
        rows.append(f"{base:02x}: " + " ".join(cells))
    return "\n".join(rows) + "\n"


class FakeSSH:
    """subprocess.run stand-in. responses: [(regex over the remote command,
    stdout | (rc, stdout) | list of those, popped per call | callable(cmd))]."""

    def __init__(self, responses):
        self.responses = [(re.compile(p), r) for p, r in responses]
        self.commands: list[str] = []
        self.argvs: list[list[str]] = []

    def __call__(self, argv, **kw):
        self.argvs.append(argv)
        if argv[0] == "scp":
            return subprocess.CompletedProcess(argv, 0, "", "")
        cmd = argv[-1]
        self.commands.append(cmd)
        for rx, resp in self.responses:
            if rx.search(cmd):
                if isinstance(resp, list):
                    resp = resp.pop(0) if len(resp) > 1 else resp[0]
                if callable(resp):
                    resp = resp(cmd)
                rc, out = resp if isinstance(resp, tuple) else (0, resp)
                return subprocess.CompletedProcess(argv, rc, out, "" if rc == 0 else "boom")
        raise AssertionError(f"unexpected command: {cmd}")


def target(responses) -> tuple[lt.LinuxTarget, FakeSSH]:
    fake = FakeSSH(responses)
    return lt.LinuxTarget("unit", runner=fake), fake


def md5(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


# --- runner ---------------------------------------------------------------------

def test_run_argv_and_check():
    t, fake = target([("true", "ok\n"), ("false", (1, ""))])
    assert t.run("true").stdout == "ok\n"
    argv = fake.argvs[0]
    assert argv[0] == "ssh" and "BatchMode=yes" in argv and "StrictHostKeyChecking=accept-new" in argv
    assert argv[-2:] == ["root@unit", "true"]
    with pytest.raises(BenchError, match="rc=1"):
        t.run("false")
    assert t.run("false", check=False).rc == 1


def test_run_timeout_is_bench_error():
    def runner(argv, **kw):
        raise subprocess.TimeoutExpired(argv, 1)
    with pytest.raises(BenchError, match="timed out"):
        lt.LinuxTarget("unit", runner=runner).run("sleep 9", timeout=1)


def test_md5_uses_offset_and_size():
    t, fake = target([("md5sum", "d41d8cd98f00b204e9800998ecf8427e  -\n")])
    t.md5("/dev/mtd1", 0x1A0000, 16)
    assert fake.commands[-1] == "tail -c +1703937 /dev/mtd1 | head -c 16 | md5sum"


# --- console ---------------------------------------------------------------------

def test_console_login_and_discover_host():
    con = FakeConsole([
        (r"^\r$", "\r\nunit login: "),
        (r"^root\r$", "root\r\nroot@unit:~# "),
        (r"ip -4 -o addr show scope global", "2: eth0    inet 10.0.0.7/24 brd 10.0.0.255 scope global eth0\r\n# "),
    ])
    lt.console_login(con)
    assert lt.discover_host(con) == "10.0.0.7"


def test_console_login_refuses_password():
    con = FakeConsole([(r"^\r$", "login: "), (r"^root\r$", "Password: ")])
    with pytest.raises(BenchError, match="password"):
        lt.console_login(con)


# --- block devices ------------------------------------------------------------------

def test_resolve_emmc_by_type_not_index():
    # SD enumerated first, eMMC second, boot areas share the card's type
    out = "mmcblk0 SD\nmmcblk1 MMC\nmmcblk1boot0 MMC\nmmcblk1boot1 MMC\n"
    t, _ = target([("device/type", out)])
    assert lt.resolve_emmc(t) == "/dev/mmcblk1"


@pytest.mark.parametrize("out", ["mmcblk0 SD\n", "mmcblk0 MMC\nmmcblk1 MMC\n"])
def test_resolve_emmc_needs_exactly_one(out):
    t, _ = target([("device/type", out)])
    with pytest.raises(BenchError, match="exactly one eMMC"):
        lt.resolve_emmc(t)


def test_root_device_resolves_dev_root():
    t, _ = target([("mountpoint -d /", "179:2\n"),
                   ("readlink", "/sys/devices/platform/soc/15c10000.mmc/mmc_host/mmc0/mmc0:aaaa/block/mmcblk0/mmcblk0p2\n")])
    assert lt.root_device(t) == "/dev/mmcblk0p2"


# --- xSPI ------------------------------------------------------------------------------

def test_mtd_write_verify(tmp_path):
    img = tmp_path / "fip.bin"
    img.write_bytes(b"\xa5" * 0x12345)
    good = md5(img.read_bytes())
    t, fake = target([("erasesize", "65536\n"), ("mtd1/size", "4194304\n"),
                      ("md5sum", f"{good}  -\n"), ("flash_erase|mtd_debug|rm -f", "")])
    assert lt.mtd_write_verify(t, 1, img, limit=lt.CM33_REGION_OFFSET) == good
    assert "flash_erase /dev/mtd1 0 2" in fake.commands
    assert "mtd_debug write /dev/mtd1 0 74565 /tmp/fip.bin" in fake.commands
    assert fake.commands[-1] == "rm -f /tmp/fip.bin"


def test_mtd_write_refuses_to_erase_into_cm33_region(tmp_path):
    img = tmp_path / "fip.bin"
    img.write_bytes(b"\0" * (lt.CM33_REGION_OFFSET + 1))
    t, fake = target([("erasesize", "65536\n")])
    with pytest.raises(ValueError, match="0x1a0000"):
        lt.mtd_write_verify(t, 1, img, limit=lt.CM33_REGION_OFFSET)
    assert not any("flash_erase" in c for c in fake.commands)


def test_mtd_write_readback_mismatch(tmp_path):
    img = tmp_path / "bl2.bin"
    img.write_bytes(b"\x01" * 100)
    good = md5(img.read_bytes())
    t, _ = target([("erasesize", "65536\n"), ("mtd0/size", "393216\n"),
                   ("md5sum", [f"{good}  -\n", "0" * 32 + "  -\n"]), ("flash_erase|mtd_debug|rm -f", "")])
    with pytest.raises(BenchError, match="readback"):
        lt.mtd_write_verify(t, 0, img)


# --- eMMC boot + EXT_CSD ----------------------------------------------------------------

EXTCSD = """Boot write protection [BOOT_WP]: 0x00
Boot bus Conditions [BOOT_BUS_CONDITIONS: 0x{a:02x}]
Boot configuration bytes [PARTITION_CONFIG: 0x{b:02x}]
Extended CSD rev 1.8 [EXT_CSD_REV: 0x08]
"""


def test_emmc_boot1_force_ro_restored_on_failure(tmp_path):
    img = tmp_path / "bl2_mmc.bin"
    img.write_bytes(b"\x02" * 512)
    t, fake = target([("md5sum", f"{md5(img.read_bytes())}  -\n"), ("force_ro|rm -f", ""),
                      (r"^dd ", (1, ""))])
    with pytest.raises(BenchError):
        lt.emmc_boot1_write_verify(t, "/dev/mmcblk1", img, 1)
    assert "echo 1 > /sys/block/mmcblk1boot1/force_ro" in fake.commands


def test_emmc_boot1_write_verify(tmp_path):
    img = tmp_path / "fip.bin"
    img.write_bytes(b"\x03" * 1000)
    good = md5(img.read_bytes())
    t, fake = target([("md5sum", f"{good}  -\n"), ("force_ro|rm -f|^dd ", "")])
    assert lt.emmc_boot1_write_verify(t, "/dev/mmcblk1", img, 0x300) == good
    assert "dd if=/tmp/fip.bin of=/dev/mmcblk1boot1 bs=512 seek=768 conv=fsync status=none" in fake.commands
    assert "tail -c +393217 /dev/mmcblk1boot1 | head -c 1000 | md5sum" in fake.commands


def test_set_boot_config_verifies():
    t, fake = target([("mmc boot", ""), ("extcsd read", EXTCSD.format(a=2, b=8))])
    lt.set_boot_config(t, "/dev/mmcblk1")
    assert fake.commands[:2] == ["mmc bootbus set single_backward x1 x8 /dev/mmcblk1",
                                 "mmc bootpart enable 1 0 /dev/mmcblk1"]
    t, _ = target([("mmc boot", ""), ("extcsd read", EXTCSD.format(a=0, b=0x48))])
    with pytest.raises(BenchError, match=r"\[179\]=0x48"):
        lt.set_boot_config(t, "/dev/mmcblk1")


def test_rootfs_write_verify_streams_and_compares(tmp_path):
    raw = bytes(range(256)) * 64
    wic = tmp_path / "rootfs.wic.gz"
    wic.write_bytes(gzip.compress(raw))
    t, fake = target([("gunzip", ""), ("rereadpt", ""), ("md5sum", f"{md5(raw)}  -\n")])
    assert lt.rootfs_write_verify(t, "/dev/mmcblk1", wic) == md5(raw)
    assert fake.commands[0] == "gunzip -c | dd of=/dev/mmcblk1 bs=4M conv=fsync status=none"
    assert fake.commands[-1] == f"tail -c +1 /dev/mmcblk1 | head -c {len(raw)} | md5sum"


# --- I2C + the N24S128 selector table ------------------------------------------------------

def test_unsealed_0x58_frame_refused():
    t, fake = target([])
    frame = gates.I2cFrame(0x58, b"\x02\x00", 16)
    with pytest.raises(ValueError, match="identity_frame"):
        lt.i2c_transfer(t, 0, frame)
    assert fake.commands == []


@pytest.mark.parametrize("write,read_len", [(b"\x06\x00\x1d", 0), (b"\x06\x00", 0), (b"\x06\x00\x00", 1)])
def test_selector_0x06_write_impossible_even_if_marked_sealed(write, read_len):
    t, fake = target([])
    forged = SimpleNamespace(addr=0x58, write=write, read_len=read_len, sealed=True)
    with pytest.raises(ValueError, match="0x06"):
        lt.i2c_transfer(t, 0, forged)
    assert fake.commands == []


def test_every_identity_frame_emits_no_0x06_write():
    t, fake = target([(r"i2ctransfer -y 0 w2@0x58 0x00 0x00 r64", _hx(b"\xff" * 64)),
                      (r"i2ctransfer -y 0 w2@0x58 0x02 0x00 r16", _hx(bytes.fromhex(UNIQUE_ID))),
                      (r"i2ctransfer -y 0 w2@0x58 0x0[46] 0x00 r1", "0xfd"),
                      (r"i2ctransfer -y 0 w\d+@0x58", "")])
    for op in gates.IdentityOp:
        payload = b"\x00" * 64 if op is gates.IdentityOp.SECURE_PAGE_WRITE else b""
        lt.i2c_transfer(t, 0, gates.identity_frame(op, payload))
    sel6 = [c for c in fake.commands if re.search(r"@0x58 0x06", c)]
    assert sel6 == ["i2ctransfer -y 0 w2@0x58 0x06 0x00 r1"]


def test_i2c_transfer_refuses_0x50_and_i2c_set_refuses_eeprom_and_clkgen():
    t, fake = target([])
    with pytest.raises(ValueError):
        lt.i2c_transfer(t, 0, gates.I2cFrame(0x50, b"\x00\x00", 1))
    for addr in (0x50, 0x58, 0x69):
        with pytest.raises(ValueError):
            lt.i2c_set(t, 0, addr, 0, 0)
    assert fake.commands == []


def test_eeprom_write_pages_chunks_and_verifies():
    data = bytes(range(128))
    t, fake = target([(r"w\d+@0x50 0x00 0x[0-7]0 0x", ""), (r"w2@0x50 0x00 0x00 r128", _hx(data))])
    lt.eeprom_write_pages(t, 0, 0, data)
    writes = [c for c in fake.commands if "until" in c]
    assert len(writes) == 8
    assert all(c.startswith("i2ctransfer -y 0 w18@0x50 ") and "|| exit 2" in c for c in writes)
    assert writes[1].split(" || ")[0].split()[4:6] == ["0x00", "0x10"]


def test_eeprom_write_pages_never_crosses_a_chunk_boundary():
    data = b"\xaa" * 20
    t, fake = target([(r"until", ""), (r"r20", _hx(data))])
    lt.eeprom_write_pages(t, 0, 8, data)
    sizes = [re.search(r"w(\d+)@", c)[1] for c in fake.commands if "until" in c]
    assert sizes == ["10", "14"]          # 8 + 12 bytes, split at 0x10
    with pytest.raises(ValueError):
        lt.eeprom_write_pages(t, 0, 0, data, page=24)


def test_eeprom_write_pages_readback_mismatch():
    t, _ = target([(r"until", ""), (r"r16", _hx(b"\xff" * 16))])
    with pytest.raises(BenchError, match="0x0000"):
        lt.eeprom_write_pages(t, 0, 0, b"\x00" * 16)


def test_secure_page_write_polls_then_verifies():
    page = bytes(range(64))
    wr = gates.identity_frame(gates.IdentityOp.SECURE_PAGE_WRITE, page)
    rd = gates.identity_frame(gates.IdentityOp.SECURE_PAGE_READ)
    t, fake = target([(r"w66@0x58", ""), (r"r64", [(1, ""), _hx(page)])])
    assert lt.secure_page_write_verify(t, 0, wr, rd) == page
    assert len([c for c in fake.commands if "r64" in c]) == 2


def test_i2c_scan_counts_uu():
    t, _ = target([("i2cdetect -y -r 8", _i2cdetect({0x1E, 0x25, 0x52}))])
    assert lt.i2c_scan(t, 8) == {0x1E, 0x25, 0x52}


def test_act88760_release_only_on_defect():
    t, fake = target([("i2cget -y -f 8 0x25 0x10", ["0x88", "0x88", "0x08"]), ("i2cset", "")])
    assert lt.act88760_gpio4_defect(t, 8)
    lt.act88760_gpio4_release(t, 8)
    assert "i2cset -y 8 0x25 0x10 0x08" in fake.commands
    t, fake = target([("i2cget -y -f 8 0x25 0x10", "0x08")])
    with pytest.raises(BenchError, match="refusing"):
        lt.act88760_gpio4_release(t, 8)
    assert not any("i2cset" in c for c in fake.commands)


def test_expected_i2c_and_check():
    preset = {"on_module": {"i2c_devices": {
        "brd_i2c": {"devices": [{"address_7bit": "0x1E"}, {"address_7bit": "0x4D", "assembled": "optional"}]},
        "e1m_i2c0": {"devices": [{"address_7bit": "0x50"}, {"address_7bit": "0x58"}]}}}}
    exp = lt.expected_i2c(preset, {"eeprom": 0, "brd": 8, "pmic": 8})
    assert exp == {8: {0x1E}, 0: {0x50, 0x58}}
    t, _ = target([("i2cdetect -y -r 0", _i2cdetect({0x40, 0x50, 0x58, 0x69})),
                   ("i2cdetect -y -r 8", _i2cdetect(set()))])
    assert lt.i2c_check(t, exp) == ["i2c-8: missing 0x1e"]


# --- clock generator (5L35023B) -------------------------------------------------------

def _clkgen_i2cget(image: bytes):
    def resp(cmd):
        m = re.search(r"i2cget -y -f \d+ 0x69 (0x[0-9a-fA-F]+)", cmd)
        return f"0x{image[int(m[1], 16)]:02x}"
    return resp


def test_clkgen_read_image_matches_fixed_up_otp():
    image = bytearray(lt.CLKGEN_OTP_IMAGE)
    image[0x21], image[0x24] = 0xC0, 0x8E
    t, _ = target([(r"i2cget -y -f \d+ 0x69 0x\w+", _clkgen_i2cget(bytes(image)))])
    got = lt.clkgen_read_image(t, 8)
    assert got == bytes(image)
    assert lt.clkgen_diff(got) == []


def test_clkgen_diff_reports_mismatch_and_missing_fixup():
    factory = bytes(lt.CLKGEN_OTP_IMAGE)  # OTP image with the U-Boot fixup NOT applied
    bad = lt.clkgen_diff(factory)
    assert any("reg 0x21" in b for b in bad) and any("reg 0x24" in b for b in bad)


def test_clkgen_diff_rejects_wrong_length():
    with pytest.raises(ValueError, match="37 bytes"):
        lt.clkgen_diff(b"\x00" * 10)


# --- DX-M1 NPU (V2M-only) --------------------------------------------------------------
# The shipping V2N image has no libgpiod/gpioset (CONFIG_GPIO_SYSFS=y only),
# so lines are driven via /sys/class/gpio. P75/PA6 = "10410000.pinctrl" chip,
# base 416, within-chip lines 61/86 -> global 477/502 (silicon-verified).

def test_sysfs_gpio_line_name():
    assert lt._sysfs_gpio_line_name(61) == "P75"
    assert lt._sysfs_gpio_line_name(86) == "PA6"
    assert lt._sysfs_gpio_line_name(52) == "P64"


def test_pinctrl_chip_base_reads_live_base_by_label():
    t, _ = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"
                                                    "/sys/class/gpio/gpiochip398 gd32-bridge-gpio\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
    ])
    assert lt._pinctrl_chip_base(t, "10410000.pinctrl") == 416


def test_pinctrl_chip_base_raises_when_label_not_found():
    t, _ = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip398 gd32-bridge-gpio\n"),
    ])
    with pytest.raises(BenchError, match="no /sys/class/gpio/gpiochip"):
        lt._pinctrl_chip_base(t, "10410000.pinctrl")


def test_sysfs_gpio_dir_uses_the_already_named_export_without_writing_export():
    # matches the real V2N board: P75/PA6 are pre-exported by the pinctrl
    # driver under their DT name; gpio<N> never appears.
    t, fake = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio477/value$", (1, "")),
        (r"^test -e /sys/class/gpio/P75/value$", (0, "")),
    ])
    assert lt._sysfs_gpio_dir(t, "10410000.pinctrl", 61) == "/sys/class/gpio/P75"
    assert not any("export" in c for c in fake.commands)


def test_sysfs_gpio_dir_exports_when_neither_form_exists_yet():
    t, fake = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio426/value$", [(1, ""), (0, "")]),
        (r"^test -e /sys/class/gpio/P12/value$", (1, "")),
        (r"^echo 426 > /sys/class/gpio/export$", ""),
    ])
    assert lt._sysfs_gpio_dir(t, "10410000.pinctrl", 10) == "/sys/class/gpio/gpio426"
    assert "echo 426 > /sys/class/gpio/export" in fake.commands


def test_sysfs_gpio_dir_raises_if_export_fails_for_a_real_reason():
    t, _ = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio426/value$", (1, "")),
        (r"^test -e /sys/class/gpio/P12/value$", (1, "")),
        (r"^echo 426 > /sys/class/gpio/export$", (1, "")),
    ])
    with pytest.raises(BenchError, match="could not export"):
        lt._sysfs_gpio_dir(t, "10410000.pinctrl", 10)


def test_dxm1_reset_pulse_is_one_ssh_round_trip():
    t, fake = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio502/value$", (1, "")),
        (r"^test -e /sys/class/gpio/PA6/value$", (0, "")),
        (r"^echo low > /sys/class/gpio/PA6/direction; sleep 0\.1; echo high > /sys/class/gpio/PA6/direction$", ""),
    ])
    lt.dxm1_reset_pulse(t, "10410000.pinctrl", 86)
    assert fake.commands[-1] == ("echo low > /sys/class/gpio/PA6/direction; "
                                 "sleep 0.1; echo high > /sys/class/gpio/PA6/direction")


def test_dxm1_uart_boot_drives_p75_via_sysfs_and_releases_it_afterward():
    t, fake = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio477/value$", (1, "")),
        (r"^test -e /sys/class/gpio/P75/value$", (0, "")),
        (r"^echo high > /sys/class/gpio/P75/direction$", ""),
        (r"^test -e /sys/class/gpio/gpio502/value$", (1, "")),
        (r"^test -e /sys/class/gpio/PA6/value$", (0, "")),
        (r"^echo low > /sys/class/gpio/PA6/direction; sleep 0\.1; echo high > /sys/class/gpio/PA6/direction$", ""),
        (r"^uart_boot -d /dev/ttySC1 -f fw_uart_boot\.bin -b 115200$", "bootloader ok\n"),
        (r"^uart_boot -d /dev/ttySC1 -F fw\.bin -U -b 115200$", "app ok\n"),
        (r"^echo low > /sys/class/gpio/P75/direction$", ""),
    ])
    out = lt.dxm1_uart_boot(t, "10410000.pinctrl", 61, 86, "/dev/ttySC1", "uart_boot",
                            "fw_uart_boot.bin", "fw.bin")
    assert out == "bootloader ok\napp ok\n"
    assert fake.commands[-1] == "echo low > /sys/class/gpio/P75/direction"


def test_dxm1_uart_boot_releases_p75_even_if_the_transfer_fails():
    t, fake = target([
        (r"^for d in /sys/class/gpio/gpiochip\*", "/sys/class/gpio/gpiochip416 10410000.pinctrl\n"),
        (r"^cat /sys/class/gpio/gpiochip416/base$", "416\n"),
        (r"^test -e /sys/class/gpio/gpio477/value$", (1, "")),
        (r"^test -e /sys/class/gpio/P75/value$", (0, "")),
        (r"^echo high > /sys/class/gpio/P75/direction$", ""),
        (r"^test -e /sys/class/gpio/gpio502/value$", (1, "")),
        (r"^test -e /sys/class/gpio/PA6/value$", (0, "")),
        (r"^echo low > /sys/class/gpio/PA6/direction; sleep 0\.1; echo high > /sys/class/gpio/PA6/direction$", ""),
        (r"^uart_boot -d /dev/ttySC1 -f fw_uart_boot\.bin -b 115200$", (1, "no ROM response")),
        (r"^echo low > /sys/class/gpio/P75/direction$", ""),
    ])
    with pytest.raises(BenchError):
        lt.dxm1_uart_boot(t, "10410000.pinctrl", 61, 86, "/dev/ttySC1", "uart_boot",
                          "fw_uart_boot.bin", "fw.bin")
    assert fake.commands[-1] == "echo low > /sys/class/gpio/P75/direction"


def test_dxm1_pcie_present_excludes_bridges_and_root_ports_by_pci_class():
    # root port only (bridge class): no endpoint
    t, _ = target([("ls /sys/bus/pci/devices", "0000:00:00.0\n"),
                   ("0000:00:00.0/class", "0x060400\n")])
    assert not lt.dxm1_pcie_present(t)
    # root port + an intermediate switch port, both bridge-class: still no endpoint
    t, _ = target([("ls /sys/bus/pci/devices", "0000:00:00.0\n0000:01:00.0\n"),
                   ("0000:00:00.0/class", "0x060400\n"),
                   ("0000:01:00.0/class", "0x060400\n")])
    assert not lt.dxm1_pcie_present(t)
    # a real (non-bridge) endpoint
    t, _ = target([("ls /sys/bus/pci/devices", "0000:00:00.0\n0000:01:00.0\n"),
                   ("0000:00:00.0/class", "0x060400\n"),
                   ("0000:01:00.0/class", "0x020000\n")])
    assert lt.dxm1_pcie_present(t)
    t, _ = target([("ls /sys/bus/pci/devices", "")])
    assert not lt.dxm1_pcie_present(t)


def test_dxm1_pcie_present_matches_vendor_id_when_given():
    t, _ = target([("ls /sys/bus/pci/devices", "0000:00:00.0\n0000:01:00.0\n"),
                   ("0000:00:00.0/class", "0x060400\n"),
                   ("0000:01:00.0/class", "0x020000\n"),
                   ("0000:01:00.0/vendor", "0x1f4b\n")])
    assert lt.dxm1_pcie_present(t, "0x1f4b")
    t, _ = target([("ls /sys/bus/pci/devices", "0000:00:00.0\n0000:01:00.0\n"),
                   ("0000:00:00.0/class", "0x060400\n"),
                   ("0000:01:00.0/class", "0x020000\n"),
                   ("0000:01:00.0/vendor", "0xdead\n")])
    assert not lt.dxm1_pcie_present(t, "0x1f4b")


# --- census --------------------------------------------------------------------------

CID = "d6" + "01" + "00" + "454d4d433031" + "10" + "12345678" + "9a" + "01"


def _census_responses(array: bytes = b"\xff" * 128):
    pmic_regs = {0x10: 0x88, 0x00: 0x03, 0x01: 0x00, 0x06: 0x00, 0x07: 0x03, 0x08: 0x00,
                 0x09: 0x00, 0x0A: 0x46, 0x0B: 0x46, 0x0C: 0x46, 0x0D: 0x46, 0x19: 0x92,
                 0x1A: 0x01, 0x1B: 0x02}

    def i2cget(cmd):
        _, _, _, bus, addr, reg = cmd.split()[:6]
        addr, reg = int(addr, 16), int(reg, 16)
        if addr == 0x52:
            return "0x10"
        if addr in lt.TPS628640_ADDRS:
            return "0x5a"
        return f"0x{pmic_regs[reg]:02x}"

    return [
        (r"devmem 0x10430300", "0x00003C06\n"), (r"devmem 0x10430304", "0x00000001\n"),
        (r"devmem 0x10430308", "0x00000002\n"),
        (r"cpuinfo_max_freq", "1800000\n"), (r"meminfo", "MemTotal:        3200000 kB\nMemFree: 1 kB\n"),
        (r"uname -r", "6.1.107-cip28\n"),
        (r"device/type", "mmcblk0 SD\nmmcblk1 MMC\nmmcblk1boot1 MMC\n"),
        (r"mmcblk1/device/cid", CID + "\n"), (r"mmcblk1/size", "30535680\n"),
        (r"extcsd read", EXTCSD.format(a=2, b=8)),
        (r"/ios", "actual clock:\t200000000 Hz\ntiming spec:\t9 (mmc HS200)\n"),
        (r"mmcblk1boot1 \| head -c 100 ", "11" * 16 + "  -\n"),
        (r"mmcblk1boot1 \| head -c 200 ", "22" * 16 + "  -\n"),
        (r"spi-nor/jedec_id", "aabbcc\n"),
        (r"mtd\*; do", "393216\n66715648\n"),
        (r"mtd0 \| head", "33" * 16 + "  -\n"), (r"tail -c \+1 /dev/mtd1 \|", "44" * 16 + "  -\n"),
        (r"tail -c \+1703937 /dev/mtd1", "55" * 16 + "  -\n"),
        (r"w2@0x58 0x02 0x00 r16", _hx(bytes.fromhex(UNIQUE_ID))),
        (r"w2@0x58 0x04 0x00 r1", "0xfd\n"), (r"w2@0x58 0x06 0x00 r1", "0x1d\n"),
        (r"w2@0x58 0x00 0x00 r64", _hx(b"\xff" * 64)),
        (r"w2@0x50 0x00 0x00 r128", _hx(array)),
        (r"i2cget", i2cget),
        (r"i2cdetect -y -r 8", _i2cdetect({0x1E, 0x25, 0x26, 0x44, 0x48, 0x4F, 0x52, 0x69, 0x70})),
        (r"eth0/", "aa:bb:cc:00:00:01\nup\n"), (r"eth1/", (1, "")),
    ]


def test_census_collects_ledger_keys_read_only():
    t, fake = target(_census_responses())
    facts, notes = lt.census(t, {"eeprom": 0, "pmic": 8, "brd": 8},
                             sizes={"bl2_mmc": 100, "fip": 200, "bl2": 300, "cm33": 400})
    assert facts["eeprom_unique_id"] == UNIQUE_ID
    assert facts["eeprom_lock_status"] == "0xfd"
    assert facts["eeprom_device_config"] == "0x1d"
    assert facts["secure_page_state"] == "blank"
    assert facts["secure_page_sha256"] == hashlib.sha256(b"\xff" * 64).hexdigest()
    assert "manifest_sha256" not in facts                      # blank array
    assert facts["soc_sys_lsi_mode"].startswith("0x3c06 (unverified")
    assert facts["cpu_khz"] == "1800000" and facts["linux_memtotal_kb"] == "3200000"
    assert facts["emmc_cid_pnm"] == "EMMC01" and facts["emmc_cid_psn"] == "0x12345678"
    assert facts["emmc_cid_mid"] == "0xd6" and facts["emmc_cid_mdt"] == "0x9a"
    assert facts["emmc_size_bytes"] == str(30535680 * 512)
    assert facts["emmc_mode"] == "mmc HS200 200000000 Hz"
    assert (facts["emmc_ext_csd_177"], facts["emmc_ext_csd_179"]) == ("0x02", "0x08")
    assert facts["emmc_boot1_bl2_md5"] == "11" * 16 and facts["emmc_boot1_fip_md5"] == "22" * 16
    assert facts["xspi_jedec_id"] == "0xaabbcc" and facts["xspi_size_bytes"] == str(393216 + 66715648)
    assert (facts["xspi_bl2_md5"], facts["xspi_fip_md5"], facts["xspi_cm33_md5"]) == \
        ("33" * 16, "44" * 16, "55" * 16)
    assert facts["act88760_gpio_regs"] == "0x10=0x88"
    assert facts["da9292_ids"] == "0x19=0x92 0x1a=0x01 0x1b=0x02"
    assert facts["tps_present"] == "0x44 0x48 0x4f"
    assert facts["tps_vout"].startswith("0x44=0x5a")
    assert facts["rtc_rv3028_reg_0x37"] == "0x10"
    assert facts["clkgen_5l35023b_regs"] == "ack at 0x69"
    assert facts["eth0_mac"] == "aa:bb:cc:00:00:01" and facts["eth0_link"] == "up"
    assert notes == ["eth1: not present"]
    # read-only: no writes of any kind reached the unit
    assert not [c for c in fake.commands if re.search(r"i2cset|flash_erase|mtd_debug write|\bdd\b|mmc boot", c)]
    assert not [c for c in fake.commands if re.search(r"w(?!2@)\d+@0x5[08]", c)]


def test_census_manifest_crc():
    body = bytes(range(0x7C))
    arr = body + zlib.crc32(body).to_bytes(4, "little")
    t, _ = target(_census_responses(arr))
    facts, notes = lt.census(t, {"eeprom": 0, "pmic": 8, "brd": 8})
    assert facts["manifest_sha256"] == hashlib.sha256(arr).hexdigest()
    assert facts["manifest_crc32"] == f"0x{zlib.crc32(body):08x}"
    assert not [n for n in notes if "crc" in n]
    assert "xspi_bl2_md5" not in facts            # no sizes -> no md5 keys


def test_parse_emmc_cid_rejects_garbage():
    with pytest.raises(ValueError):
        lt.parse_emmc_cid("xyz")
