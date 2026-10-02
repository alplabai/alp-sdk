# SPDX-License-Identifier: Apache-2.0
"""functional_test: every check against a fake unit (#2624). No hardware.

``Unit`` plays the board for the ONE remote invocation: it reads the script the step pushed,
takes the check names from it and prints, per check, the scripted answer in the script's own
frame. An answer the catalogue has no check for, or a check with no scripted answer, is an
AssertionError, so the catalogue and this file cannot drift apart.

``GOOD`` holds a healthy unit's answers and ``BAD`` one wrong answer per check: the
mutation-style table (change the fake's answer to a wrong value, the check must fail). Where
an answer is a line from a real provisioning log it is marked ``real``; the rest are the
format the judge ASSUMES and a first bench run must confirm (docs/provisioning-v2n.md).
"""

from __future__ import annotations

import re
import shlex
import shutil
import subprocess
import time
from pathlib import Path

import pytest
import yaml
from provision import functest, ledger_out, steps
from provision import linux_target as lt
from provision.bench import BenchError

from .provision_fakes import FakeLinux, FakePower
from .test_provision_steps import CATALOGUE, SERIAL, SKU, _bench, _ctx, _statuses

FIXTURES = REPO = Path(__file__).resolve().parent / "fixtures" / "provision"
ALL_FIXTURES = {"eth1_cable": True, "wifi_ap": {"ssid": "bench-ap", "min_signal_dbm": -70},
                "ble_advertiser": {"address": "AA:BB:CC:DD:EE:FF"}, "dxm1_model": "/usr/share/model.dxnn",
                "usb_stick": True, "sd_card": True, "rtc_backup": True, "ina228_rework": True,
                "camera": "imx", "can_loopback": True, "uart_loopback": "/dev/ttySC1"}
PMIC = {"devices": {"act88760": {"bus": "pmic", "addr": 0x25, "registers": [
    {"reg": 0x40, "expect": 0x80, "mask": 0x80}, {"reg": 0x10, "expect": 0x08}]}}}
DMESG = "\n".join([                                               # real lines; the <level> is assumed
    "<6>[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x412fd050]",
    "<3>[    1.049170] irq 14: nobody cared (try booting with the \"irqpoll\" option)",
    "<4>[    1.078066]  dump_backtrace.part.0+0xdc/0xf0",
    "<0>[    1.227549] Disabling IRQ #14",
    "<3>[    2.302188] rtc-rv3028 8-0052: hctosys: unable to read the hardware clock",
    "<3>[    2.300654] rz-sci 12801c00.serial: Failed to create device link (0x180) with 8-0070",
    "<6>[    2.599772] mmc0: new HS200 MMC card at address 0001",
    "<4>[    7.046497] brcmfmac mmc2:0001:1: Direct firmware load for cypress/x.trxse failed with error -2",
])


def _hx(data: bytes) -> str:
    return " ".join(f"0x{b:02x}" for b in data)


def _good(ctx) -> dict[str, str]:
    from alp_eth_mac import derive_both_macs
    m0, m1 = (m.lower() for m in derive_both_macs(SERIAL))
    manifest = steps._build_blob(ctx, "program_eeprom.py", "manifest.bin")
    return {
        "boot_source": "emmc=mmcblk0\nroot=mmcblk0p2",
        "cpu_count": "4",
        "mem_total": "3234812",                                   # real
        "kernel_release": "6.1.141-cip43-yocto-standard",         # real
        "sku": SKU,
        "emmc_size": "30576640",                                  # real (15655239680 B)
        "emmc_health": "eMMC Life Time Estimation A [EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_A]: 0x01\n"
                       "eMMC Life Time Estimation B [EXT_CSD_DEVICE_LIFE_TIME_EST_TYP_B]: 0x01\n"
                       "eMMC Pre EOL information [EXT_CSD_PRE_EOL_INFO]: 0x01",
        "emmc_mode": "timing spec:\t9 (mmc HS200)\nhs200_failed=0",
        "emmc_read": "64+0 records out\na=20.10 b=20.90",
        "xspi": 'dev:    size   erasesize  name\nmtd0: 00060000 00001000 "bl2"\nmtd1: 01fa0000 00001000 "fip"\n'
                "jedec=1122aa",
        "eth_phy_id": "end0 0x001cc916\nend1 0x001cc916",          # real IDs
        "eth_mac": f"end0 {m0}\nend1 {m1}",
        "eth0_link": "if=end0 carrier=1 speed=100 duplex=full\nping=ok\nrx_delta=4",
        "eth1_link": "if=end1 carrier=1 speed=1000 duplex=full\nping=ok\nrx_delta=2",
        "wifi_present": "wlan0=present\nsdio=mmc2:0001:1\n[    7.966228] brcmfmac: brcmf_c_preinit_dcmds: "
                        "Firmware: BCM55500/1 wl0: Nov 15 2024 06:07:11 version 28.10.387.10 (0702395)",   # real
        "wifi_regdomain": "global\ncountry 00: DFS-UNSET\n\t(2402 - 2472 @ 40), (N/A, 20), (N/A)",
        "wifi_scan": "BSS 02:00:00:00:00:01(on wlan0)\n\tsignal: -48.00 dBm\n\tSSID: bench-ap\n"
                     "BSS 02:00:00:00:00:02(on wlan0)\n\tsignal: -80.00 dBm\n\tSSID: other",
        "bt_hci": "HIL_BT_HCI_OK\nHIL_BT_UP_OK\n\tBD Address: 11:22:33:44:55:66  ACL MTU: 1021:8  SCO MTU: 64:1",
        "bt_scan": "AA:BB:CC:DD:EE:FF (unknown)\nAA:BB:CC:DD:EE:FF bench-beacon\nLE Scan ...",
        "dxm1_pcie": "present=1 device=0x0000 width=2 speed=8.0_GT/s_PCIe\ndriver=dx_dma_pcie",
        "dxm1_runtime": "dev=ok\nservice=active\n" + (FIXTURES / "dxrt-cli-s.txt").read_text(encoding="utf-8"),
        "dxm1_inference": "loops 30\nFPS : 412.3\nrc=0",
        "drpai": "driver=bound\n/dev/drpai0",
        "gpu": "driver=bound\n/dev/mali0",
        "rtc_device": "rtc-rv3028 8-0052",
        "rtc_ticks": "a=0x58 b=0x00",                             # BCD, across the minute
        "rtc_time_set": "1790000000",
        "rtc_backup_mode": "0x1c",
        "rtc_retention": f"epoch={int(time.time())}\nboot=boot-now\nhctosys_failed=0",
        "board_temp": "0x2e 0xd0",                                 # 46.8 degC (the temperature one unit read)
        "secure_element": "HIL_OPTIGA_I2C_STATE 0x08 0x80 0x00 0x00\nHIL_OPTIGA_ACK",
        "gd32_bridge": "0x00 0x00 0x0e 0x00 0xcf 0xa7",           # real reply: protocol 0.14.0
        "gd32_gpiochip": "lines=22",
        "i2c_rv3028c7_52": "0x33",
        "i2c_act8760_25": "0x08",
        "i2c_act8760_26": "0x00",
        "i2c_da9292_1e": "0xea",                                   # real
        "i2c_tps628640_48": "0x5a",                                # real
        "i2c_tps628640_44": "0x82",
        "i2c_tps628640_4f": "0x14",
        "pmic_registers": "R act88760 0x25 0x40 0x80\nR act88760 0x25 0x10 0x08",
        "thermal": "Z thermal_zone0 46000\nZ thermal_zone1 47500",
        "cm33_firmware": "0123456789abcdef0123456789abcdef  -",
        "openamp_uio": "rsctbl\nmhu-shm\nvring-ctl0\nvring-ctl1\nvring-shm0\nvring-shm1\nmhu-uio",
        "usb_host": "4",
        "usb_device": "dev=sda\n4+0 records out",
        "sd_host": "driver=bound",
        "sd_card": "dev=mmcblk1\n4+0 records out",
        "systemd_failed": "systemd-networkd-wait-online.service loaded failed failed Wait for Network",
        "dmesg_fatal": DMESG,
        "gpio_keys": "gpio-keys",
        "eeprom_manifest": _hx(manifest),
        "secure_page": _hx(bytes(range(64))),
        "carrier_bmi323_68": "0x00 0x00 0x43 0x00",
        "carrier_icm42670_69": "0x67",
        "carrier_bmp581_47": "0x50",                               # real
        "carrier_tcal9538_73": "0xff",
        "carrier_tcal9538_71": "0x3f",
        "carrier_ina236_40": "0x54 0x49",                          # real
        "carrier_ina236_41": "0x54 0x49",
        "carrier_ina236_49": "0x54 0x49",
        "carrier_ina228_42": "0x54 0x49",
        "rail_3v3": "0x41 0x27\n0x08 0x0f\n0x0f 0xa0",              # 3.301 V, 0.5 A through 20 mOhm
        "rail_1v8": "0x41 0x27\n0x04 0x65\n0x00 0x50",              # 1.8 V
        "rail_5v": "0x06 0x40 0x00",                               # 5.0 V
        "audio": "bound=2\naplay_rc=0\nelapsed=2.01",
        "display_dsi": "card0-DSI-1 connected",
        "camera": "rzg2l-csi2 16000400.csi2\nimx219 2-0010",
        "can_loopback": "id=0x123 data=a0a1a2a3a4a5a6a7\nrc=0",
        "uart_loopback": "got=414c50\nrc=0",
    }


# One wrong answer per check (the mutation table). dmesg_clean and the host-side checks are below.
BAD = {
    "boot_source": "emmc=mmcblk0\nroot=mmcblk1p2",                 # still running from the microSD
    "cpu_count": "3",
    "mem_total": "1947000",                                        # a 2 GiB DDR config on a 4 GiB SKU
    "kernel_release": "5.10.201-cip41-yocto-standard",
    "sku": "E1M-V2N101",
    "emmc_size": "15269888",                                       # an 8 GB part
    "emmc_health": "eMMC Life Time Estimation A [x]: 0x03\neMMC Life Time Estimation B [x]: 0x01\n"
                   "eMMC Pre EOL information [x]: 0x01",
    "emmc_mode": "timing spec:\t1 (mmc high-speed)\nhs200_failed=0",
    "emmc_read": "64+0 records out\na=20.10 b=40.10",              # 3 MiB/s
    "xspi": 'mtd0: 00060000 00001000 "bl2"\njedec=ffffff',
    "eth_phy_id": "end0 0x001cc916\nend1 0x001cc878",
    "eth_mac": "end0 02:00:00:00:00:01\nend1 02:00:00:00:00:02",     # the eMMC-CID fallback MAC
    "eth0_link": "if=end0 carrier=1 speed=10 duplex=half\nping=ok\nrx_delta=4",
    "eth1_link": "if=end1 carrier=0 speed= duplex=\nping=fail\nrx_delta=0",
    "wifi_present": "sdio=\n",
    "wifi_regdomain": "global\ncountry 98: DFS-UNSET",
    "wifi_scan": "BSS 02:00:00:00:00:02(on wlan0)\n\tsignal: -80.00 dBm\n\tSSID: other",
    "bt_hci": "HIL_BT_HCI_OK\nHIL_BT_DOWN",
    "bt_scan": "01:02:03:04:05:06 someone-else\nLE Scan ...",
    "dxm1_pcie": "present=1 device=0x0001 width=2 speed=8.0_GT/s_PCIe\ndriver=dx_dma_pcie",
    "dxm1_runtime": "dev=ok\nservice=failed\n * FW version          : v2.4.0",
    "dxm1_inference": "error: device busy\nrc=1",
    "drpai": "",
    "gpu": "driver=bound",
    "rtc_device": "rtc-rtca3 11c00800.rtc",
    "rtc_ticks": "a=0x12 b=0x12",                                  # oscillator stopped
    "rtc_time_set": "",
    "rtc_backup_mode": "0x10",                                     # real: what one unit reads today
    "rtc_retention": "epoch=\nboot=boot-now\nhctosys_failed=0",
    "board_temp": "0x7f 0xf0",                                     # 127.9 degC
    "secure_element": "",
    "gd32_bridge": "0x00 0x00 0x0d 0x00 0x9c 0xf2",                 # protocol 0.13.0
    "gd32_gpiochip": "lines=8",
    "i2c_rv3028c7_52": "Error: Read failed",                       # (unread: no ID value is pinned)
    "i2c_act8760_25": "Error: Read failed",
    "i2c_act8760_26": "Error: Read failed",
    "i2c_da9292_1e": "0x00",
    "i2c_tps628640_48": "0x5b",
    "i2c_tps628640_44": "Error: Read failed",
    "i2c_tps628640_4f": "Error: Read failed",
    "pmic_registers": "R act88760 0x25 0x40 0x00\nR act88760 0x25 0x10 0x08",
    "thermal": "Z thermal_zone0 46000\nZ thermal_zone1 -274000",
    "cm33_firmware": "ecb99e6ffea7be1e5419350f725da86b  -",          # md5 of 64 KiB of 0xff
    "openamp_uio": "rsctbl\nmhu-shm",
    "usb_host": "2",
    "usb_device": "",
    "sd_host": "",
    "sd_card": "dev=mmcblk1\ndd: /dev/mmcblk1: Input/output error",
    "systemd_failed": "dxrt.service loaded failed failed DXRT daemon",
    "dmesg_fatal": DMESG + "\n<3>[   61.000000] i2c i2c-8: SCL is stuck low, exit recovery",   # real line
    "gpio_keys": "",
    "eeprom_manifest": _hx(b"\xff" * 128),
    "secure_page": _hx(b"\xff" * 64),
    "carrier_bmi323_68": "0x00 0x00 0x67 0x00",
    "carrier_icm42670_69": "0x43",
    "carrier_bmp581_47": "0x51",
    "carrier_tcal9538_73": "Error: Read failed",
    "carrier_tcal9538_71": "Error: Read failed",
    "carrier_ina236_40": "0x00 0x00",
    "carrier_ina236_41": "0xff 0xff",
    "carrier_ina236_49": "0x54 0x48",
    "carrier_ina228_42": "0x00 0x00",
    "rail_3v3": "0x41 0x27\n0x07 0x08\n0x0f 0xa0",                  # 2.88 V
    "rail_1v8": "0x41 0x27\n0x04 0x65\n0x7f 0xff",                  # 4.1 A through the shunt
    "rail_5v": "0x05 0x00 0x00",                                   # 4.0 V
    "audio": "bound=1\naplay_rc=0\nelapsed=2.01",
    "display_dsi": "",
    "camera": "rzg2l-csi2 16000400.csi2",
    "can_loopback": "timed out\nrc=1",
    "uart_loopback": "got=\nrc=1",
}
UNREAD_NOT_FAIL = {"i2c_rv3028c7_52", "i2c_act8760_25", "i2c_act8760_26", "i2c_tps628640_44",
                   "i2c_tps628640_4f", "carrier_tcal9538_73", "carrier_tcal9538_71"}


class Unit(FakeLinux):
    """The board for the functional test: answers the pushed script, frame for frame."""

    def __init__(self, answers: dict):
        super().__init__()
        self.answers = dict(answers)
        self.script = ""

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None):
        self.commands.append(cmd)
        if cmd == f"sh {functest.REMOTE_SCRIPT}":
            self.script = self.files[functest.REMOTE_SCRIPT].decode()
            names = re.findall(r'(?m)^cat > "\$D/(\w+)\.sh" <<', self.script)
            missing = [n for n in names if n not in self.answers]
            assert not missing, f"no scripted answer for {missing}"
            out = []
            for n in names:
                a = self.answers[n]
                rc, text = a if isinstance(a, tuple) else ("0", a)
                out.append(f"@@ALPFT {n} {rc}\n{text}\n@@ALPFT-END {n}")
            return lt.CmdResult(0, "\n".join(out) + "\n", "")
        if cmd in ("true", f"rm -f {functest.REMOTE_SCRIPT}"):
            return lt.CmdResult(0, "", "")
        raise AssertionError(f"functional_test ran an unexpected command: {cmd!r}")


class MeteredPower(FakePower):
    amps = 0.27                                                    # measured idle with the DX-M1: 0.26..0.29 A
    volts = 15.0                                                   # the supply those figures were taken at

    def current(self):
        if isinstance(self.amps, Exception):
            raise self.amps
        return self.amps

    def voltage(self):
        if isinstance(self.volts, Exception):
            raise self.volts
        return self.volts


def _setup(tmp_path, answers=None, fixtures=ALL_FIXTURES, carrier="e1m-x-evk", execute=True, **kw):
    bench = _bench()
    bench.power = MeteredPower()
    bench.raw = {"functional_test": {"carrier": carrier, "fixtures": dict(fixtures)}}
    ctx = _ctx(tmp_path, bench=bench, execute=execute, expected_registers=PMIC, **kw)
    ctx.facts["rtc_set_boot_id"] = "boot-before"
    unit = Unit({**_good(ctx), **(answers or {})})
    ctx.linux = unit
    return ctx, unit


def _run(ctx):
    return steps.FunctionalTest().run(ctx)


def _val(res, name):
    return res.evidence[f"test_ft_{name}"]


def test_names_of_this_file_match_the_catalogue(tmp_path):
    ctx, _ = _setup(tmp_path)
    names = {c.name for c in functest.build(ctx)}
    on_unit = {c.name for c in functest.build(ctx) if c.cmd}
    assert set(_good(ctx)) == on_unit, set(_good(ctx)) ^ on_unit
    assert set(BAD) == on_unit, set(BAD) ^ on_unit
    assert names - on_unit == {"dmesg_clean", "supply_power_idle"}


# --- pass ---------------------------------------------------------------------------------

def test_a_good_unit_passes_every_check_in_one_remote_invocation(tmp_path):
    ctx, unit = _setup(tmp_path)
    res = _run(ctx)
    not_pass = {k: v for k, v in res.evidence.items() if k.startswith("test_") and not v.startswith("pass")}
    assert all(k.startswith("test_ft_") or k == "test_functional" for k in res.evidence if k.startswith("test_"))
    assert not_pass == {}
    assert res.status == "done" and res.evidence["test_functional"] == "pass"
    assert [c for c in unit.commands if c.startswith("sh ")] == [f"sh {functest.REMOTE_SCRIPT}"]
    assert functest.REMOTE_SCRIPT not in unit.files or unit.commands[-1] == f"rm -f {functest.REMOTE_SCRIPT}"
    # measured values ride along as evidence
    assert _val(res, "supply_power_idle") == "pass (4.05 W: 15.00 V x 0.270 A)"
    assert res.evidence["psu_voltage_v"] == "15.000" and res.evidence["psu_current_a"] == "0.270"
    assert _val(res, "board_temp") == "pass (46.8125 degC)"
    assert _val(res, "gd32_bridge") == "pass (protocol 0.14.0)"
    assert _val(res, "rail_3v3") == "pass (3.301 V, 0.5 A)"
    assert _val(res, "rail_5v") == "pass (5 V)"
    assert _val(res, "dxm1_runtime") == "pass (firmware 2.4.0)"
    assert _val(res, "eth_phy_id") == "pass (end0=0x001cc916 end1=0x001cc916)"
    assert _val(res, "rtc_ticks") == "pass (+2 s)"


# --- fail: the mutation table -----------------------------------------------------------------

@pytest.mark.parametrize("name", sorted(BAD))
def test_a_wrong_answer_fails_exactly_that_check(tmp_path, name):
    ctx, _ = _setup(tmp_path, {name: BAD[name]})
    x = functest.load_expect()
    res = _run(ctx)
    want = "unread (" if name in UNREAD_NOT_FAIL else "fail ("
    assert _val(res, name).startswith(want), _val(res, name)
    others = {k: v for k, v in res.evidence.items()
              if k.startswith("test_ft_") and k not in (f"test_ft_{name}", "test_ft_dmesg_clean")
              and not v.startswith("pass")}
    assert others == {}                                            # no collateral: one wrong answer, one check
    if name in x["informational"]:
        assert res.status == "done" and res.evidence["test_functional"] == "pass"
        assert f"informational: {name}" in res.detail
    else:
        assert res.status == "failed"
        assert res.evidence["test_functional"] == f"fail ({name})"
        assert f"{name}: {_val(res, name)}" in res.detail


def test_the_bad_table_really_differs_from_the_good_one(tmp_path):
    ctx, _ = _setup(tmp_path)
    good = _good(ctx)
    assert all(BAD[n] != good[n] for n in BAD)


def test_dmesg_clean_fails_on_an_unlisted_error_line_and_is_informational(tmp_path):
    line = "<3>[   12.000000] renesas_sdhi_internal_dmac 15c10000.mmc: timeout waiting for hardware interrupt"
    ctx, _ = _setup(tmp_path, {"dmesg_fatal": DMESG + "\n" + line})
    res = _run(ctx)
    assert _val(res, "dmesg_clean").startswith("fail (1 unexpected error line(s), first: ")
    assert "timeout waiting for hardware interrupt" in _val(res, "dmesg_clean")
    assert _val(res, "dmesg_fatal").startswith("pass")
    assert res.status == "done"                                    # informational until the allowlist is benched


def test_dmesg_clean_without_levels_falls_back_to_error_words(tmp_path):
    plain = "\n".join(ln.split(">", 1)[1] for ln in DMESG.splitlines())
    ctx, _ = _setup(tmp_path, {"dmesg_fatal": plain})
    assert _val(_run(ctx), "dmesg_clean").startswith("pass")
    ctx, _ = _setup(tmp_path / "b", {"dmesg_fatal": plain + "\n[   12.0] foo 1-0001: probe failed with error -5"})
    assert _val(_run(ctx), "dmesg_clean").startswith("fail (")


@pytest.mark.parametrize("amps, volts, want", [
    (0.45, 15.0, "fail (6.75 W outside 3.3..4.95 W)"),                     # the PHY-regulator fault
    (0.12, 15.0, "fail (1.8 W outside 3.3..4.95 W)"),
    (0.34, 12.0, "pass (4.08 W: 12.00 V x 0.340 A)"),                      # the same power at another supply voltage
    (0.27, 12.0, "fail (3.24 W outside 3.3..4.95 W)"),                     # ... and 0.27 A is NOT fine at 12 V
    (float("nan"), 15.0, "unread (current is not a finite number: nan)"),
    (BenchError("PSU: no reply"), 15.0, "unread (PSU: no reply)"),
    (0.27, None, "unread (supply voltage not readable: None)"),
    (0.27, BenchError("MEAS:VOLT? CH1: not a number: 'ERR'"),
     "unread (supply voltage not readable: MEAS:VOLT? CH1: not a number: 'ERR')"),
    (None, 15.0, "skipped (no fixture: a supply that measures current (bench.yaml power.kind scpi))")])
def test_supply_power_band_needs_the_voltage(tmp_path, amps, volts, want):
    ctx, _ = _setup(tmp_path)
    ctx.bench.power.amps, ctx.bench.power.volts = amps, volts
    res = _run(ctx)
    assert _val(res, "supply_power_idle") == want
    assert (res.status == "failed") == want.startswith(("fail", "unread"))


def test_manifest_and_secure_page_are_compared_with_the_ledger_copies(tmp_path):
    ctx, unit = _setup(tmp_path)
    ctx.unit_dir.mkdir(parents=True, exist_ok=True)
    (ctx.unit_dir / f"{SERIAL}.manifest.bin").write_bytes(steps._build_blob(ctx, "program_eeprom.py", "manifest.bin"))
    (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").write_bytes(bytes(range(64)))
    res = _run(ctx)
    assert _val(res, "eeprom_manifest") == "pass (equals the committed manifest)"
    assert _val(res, "secure_page") == "pass (equals the staged secure page)"
    (ctx.unit_dir / f"{SERIAL}.secure-page.staged.bin").write_bytes(bytes(64))
    assert _val(_run(ctx), "secure_page").startswith("fail (the secure page differs")


def test_a_valid_manifest_of_another_unit_fails(tmp_path):
    ctx, _ = _setup(tmp_path)
    other = bytearray(steps._build_blob(ctx, "program_eeprom.py", "manifest.bin"))
    other[56:68] = b"2030W01-0001"
    other[0x7C:0x80] = functest.zlib.crc32(bytes(other[:0x7C])).to_bytes(4, "little")
    ctx.linux.answers["eeprom_manifest"] = _hx(other)
    assert _val(_run(ctx), "eeprom_manifest") == f"fail (manifest is for {SKU} 2030W01-0001)"


def test_rtc_retention_needs_a_set_before_and_a_reboot_since(tmp_path):
    ctx, unit = _setup(tmp_path)
    ctx.facts.pop("rtc_set_boot_id")
    assert "was not set before the last cold cycle" in _val(_run(ctx), "rtc_retention")
    ctx.facts["rtc_set_boot_id"] = "boot-now"
    assert _val(_run(ctx), "rtc_retention") == "fail (no reboot since the RTC was set: nothing proven)"
    ctx.facts["rtc_set_boot_id"] = "boot-before"
    unit.answers["rtc_retention"] = f"epoch={int(time.time()) - 3600}\nboot=boot-now\nhctosys_failed=0"
    assert "from the host clock after the cold cycles" in _val(_run(ctx), "rtc_retention")
    # the clock reads fine now, but this boot's kernel could not read it at start-up: it was lost
    unit.answers["rtc_retention"] = f"epoch={int(time.time())}\nboot=boot-now\nhctosys_failed=1"
    assert "hctosys: unable to read the hardware clock" in _val(_run(ctx), "rtc_retention")


def test_the_dxm1_firmware_version_comes_from_the_bundle(tmp_path):
    ctx, _ = _setup(tmp_path)
    ctx.bundle["components"].append({"role": "dxm1_fw", "file": "x", "version": "v2.5.0"})
    assert _val(_run(ctx), "dxm1_runtime") == "fail (DX-M1 firmware 2.4.0, want 2.5.0)"


def test_reference_ap_must_be_seen_strongly_enough(tmp_path):
    ctx, unit = _setup(tmp_path)
    unit.answers["wifi_scan"] = "BSS 02:00:00:00:00:01(on wlan0)\n\tsignal: -82.00 dBm\n\tSSID: bench-ap"
    assert _val(_run(ctx), "wifi_scan") == "fail (reference AP at -82 dBm < -70 dBm)"


# --- unread -----------------------------------------------------------------------------------

def test_a_timed_out_check_is_unread_and_fails_the_step(tmp_path):
    ctx, unit = _setup(tmp_path)
    unit.answers = {n: ("T", "") for n in unit.answers}
    res = _run(ctx)
    on_unit = [c for c in functest.build(ctx) if c.cmd]
    for c in on_unit:
        assert _val(res, c.name) == f"unread (timed out after {c.timeout_s} s)", c.name
    assert _val(res, "dmesg_clean") == "unread (timed out after 5 s)"
    assert res.status == "failed" and res.evidence["test_functional"].startswith("fail (")


@pytest.mark.parametrize("name, answer, want", [
    ("wifi_scan", "ALPUNREAD missing tool: iw", "unread (missing tool: iw)"),
    ("emmc_health", "ALPUNREAD missing tool: mmc", "unread (missing tool: mmc)"),
    ("emmc_health", "", "unread (no life time estimation a in the output: '')"),
    ("emmc_mode", "hs200_failed=0", "unread (mmc ios not readable (debugfs not mounted?))"),
    ("eth_phy_id", "end0 0x001cc916\nend1 errno 22", "unread (1 of 2 ports readable: 'end0 0x001cc916 end1 errno 22')"),
    ("board_temp", "Error: Read failed", "unread (no reply: 'Error: Read failed')"),
    ("gd32_bridge", "Error: Read failed", "unread (i2ctransfer returned 0 bytes, wanted 6: Error: Read failed)"),
    ("rtc_ticks", "ALPUNREAD Error: Read failed", "unread (Error: Read failed)"),
    ("audio", "ALPUNREAD missing tool: aplay", "unread (missing tool: aplay)"),
    ("rail_3v3", "0x41 0x27", "unread (want 3 register reads, got 1: '0x41 0x27')"),
    ("pmic_registers", "R act88760 0x25 0x40 Error: Read failed\nR act88760 0x25 0x10 0x08",
     "unread (act88760 reg 0x40: Error: Read failed)"),
])
def test_unreadable_answers_are_unread_never_pass(tmp_path, name, answer, want):
    ctx, _ = _setup(tmp_path, {name: answer})
    res = _run(ctx)
    assert _val(res, name) == want
    assert res.status == "failed" and name in res.evidence["test_functional"]


def test_a_script_that_dies_early_leaves_the_rest_unread(tmp_path):
    ctx, unit = _setup(tmp_path)
    real = unit.run
    unit.run = lambda cmd, **kw: (lt.CmdResult(255, "@@ALPFT boot_source 0\nemmc=mmcblk0\nroot=mmcblk0p2\n"
                                               "@@ALPFT-END boot_source\n", "") if cmd.startswith("sh ") else real(cmd, **kw))
    res = _run(ctx)
    assert _val(res, "boot_source").startswith("pass")
    assert _val(res, "cpu_count") == "unread (no result: the script did not get to this check)"
    assert res.status == "failed"


def test_reasons_are_single_line_and_bounded(tmp_path):
    ctx, _ = _setup(tmp_path, {"dxm1_inference": "line one\nline\ttwo   " + "x" * 600 + "\nrc=3"})
    v = _val(_run(ctx), "dxm1_inference")
    assert v.startswith("fail (run_model rc=3: ") and "\n" not in v and "\t" not in v and "  " not in v
    assert len(v) <= functest.REASON_MAX + len("fail ()")


def test_unread_uses_the_tools_helper():
    assert functest._value("unread", "a\n  b") == lt.unread("a\n  b") == "unread (a b)"


# --- skipped: fixtures ------------------------------------------------------------------------

FIXTURE_CHECKS = {"eth1_link": "eth1_cable", "wifi_scan": "wifi_ap", "bt_scan": "ble_advertiser",
                  "dxm1_inference": "dxm1_model", "rtc_retention": "rtc_backup", "usb_device": "usb_stick",
                  "sd_card": "sd_card", "carrier_ina228_42": "ina228_rework", "rail_5v": "ina228_rework",
                  "camera": "camera", "can_loopback": "can_loopback", "uart_loopback": "uart_loopback"}


def test_every_fixture_check_is_skipped_by_default_and_never_runs(tmp_path):
    ctx, unit = _setup(tmp_path, fixtures={})
    assert {c.name: c.fixture for c in functest.build(ctx) if c.fixture} == FIXTURE_CHECKS
    for n in FIXTURE_CHECKS:
        unit.answers.pop(n)                                        # an answer would mean it ran
    res = _run(ctx)
    for n, f in FIXTURE_CHECKS.items():
        assert _val(res, n) == f"skipped (no fixture: {f})"
        assert f"/{n}.sh" not in unit.script
    assert res.status == "done" and res.evidence["test_functional"] == "pass"
    assert "skipped: " in res.detail and "camera" in res.detail      # visible, not silent


def test_without_a_carrier_the_carrier_checks_are_one_visible_skip(tmp_path):
    ctx, unit = _setup(tmp_path, carrier=None)
    for n in [n for n in unit.answers if n.startswith(("carrier_", "rail_"))] + ["audio", "display_dsi", "camera",
                                                                             "can_loopback", "uart_loopback"]:
        unit.answers.pop(n)
    res = _run(ctx)
    assert _val(res, "carrier") == "skipped (no fixture: bench.yaml functional_test.carrier)"
    assert not [k for k in res.evidence if k.startswith(("test_ft_carrier_", "test_ft_rail_"))]
    assert res.status == "done"


def test_not_fitted_and_fixture_gated_carrier_devices(tmp_path):
    ctx, _ = _setup(tmp_path, fixtures={})
    names = {c.name for c in functest.build(ctx)}
    assert "carrier_ina236_48" not in names                         # removed: 0x48 is the amplifiers' address
    assert {"carrier_ina236_40", "carrier_ina236_41", "carrier_ina236_49", "carrier_ina228_42"} <= names


def test_a_v2n_unit_has_no_dxm1_checks(tmp_path):
    from .test_provision_steps import _bundle
    ctx, _ = _setup(tmp_path, bundle=_bundle(tmp_path, sku="E1M-V2N101", family="v2n"), sku="E1M-V2N101",
                    preset_sku="E1M-V2N101")
    names = {c.name for c in functest.build(ctx)}
    assert not {n for n in names if n.startswith("dxm1_")}
    assert "i2c_tps628640_48" not in names                           # the DEEPX rails are V2M-only


def test_a_missing_expected_value_is_unread_for_a_blocking_check_and_skipped_for_an_informational_one(tmp_path):
    x = functest.load_expect()
    x.pop("kernel_release_regex")
    x["supply_power_idle_w"] = {}
    x["rtc_bsm_enabled"] = None
    ctx, _ = _setup(tmp_path, functest_expect=x)
    res = _run(ctx)
    assert _val(res, "kernel_release") == "unread (no expected value: kernel_release_regex)"
    assert _val(res, "supply_power_idle") == "unread (no expected value: supply_power_idle_w.v2n-m1)"
    assert _val(res, "rtc_backup_mode") == "skipped (no expected value: rtc_bsm_enabled)"      # informational
    assert res.status == "failed"
    assert res.evidence["test_functional"] == "fail (supply_power_idle, kernel_release)"


# --- record / ship check ------------------------------------------------------------------------

def _shippable_ledger(tmp_path):
    cat = {**CATALOGUE, "keys": {**CATALOGUE["keys"],
                                 "test_*": {"group": "campaign", "source": "", "mode": "auto", "ship_required": False}}}
    cat["keys"].pop("eeprom_unique_id")
    root = tmp_path / "ledger-ft"
    (root / "schema").mkdir(parents=True)
    (root / "schema" / "v2n.keys.yaml").write_text(yaml.safe_dump(cat), encoding="utf-8")
    unit = root / SKU / f"{SERIAL}.unit.yaml"
    unit.parent.mkdir(parents=True)
    unit.write_text("disposition: ship\n", encoding="utf-8")
    return root, unit


def test_a_failing_functional_test_blocks_record(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    ctx, _ = _setup(tmp_path, {"eth_phy_id": BAD["eth_phy_id"]}, ledger_root=root)
    res = steps.run_steps(ctx, only=["functional_test", "hil_smoke", "record"])
    st = _statuses(res)
    assert st["functional_test"] == "failed" and "hil_smoke" not in st       # the run stops, record still runs
    assert "ship check: blocked: test_functional: fail (eth_phy_id)" in res[-1].detail
    text = unit.read_text(encoding="utf-8")
    assert "test_functional: fail (eth_phy_id)" in text
    assert "test_ft_eth_phy_id: fail (PHY ID {'end1': '0x001cc878'}, want 0x001cc916)" in text
    assert "test_ft_cpu_count: pass (4)" in text                                 # every check is in the unit record
    assert ctx.state["steps"]["functional_test"]["status"] == "failed"


def test_fixture_skipped_tests_do_not_block_record_and_stay_visible(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    ctx, unit_fake = _setup(tmp_path, fixtures={}, ledger_root=root)
    res = steps.run_steps(ctx, only=["functional_test", "record"])
    assert _statuses(res)["functional_test"] == "done", res[1].detail
    assert "ship check: SHIPPABLE" in res[-1].detail
    text = unit.read_text(encoding="utf-8")
    assert "test_functional: pass" in text
    assert "test_ft_wifi_scan: skipped (no fixture: wifi_ap)" in text
    assert "test_ft_eth1_link: skipped (no fixture: eth1_cable)" in text


def test_an_informational_failure_does_not_block_record(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    ctx, _ = _setup(tmp_path, {"rtc_backup_mode": BAD["rtc_backup_mode"], "cm33_firmware": BAD["cm33_firmware"]},
                    ledger_root=root)
    res = steps.run_steps(ctx, only=["functional_test", "record"])
    assert "ship check: SHIPPABLE" in res[-1].detail
    text = unit.read_text(encoding="utf-8")
    assert "test_ft_rtc_backup_mode: fail (backup switchover disabled (reg 0x37=0x10, BSM=0b00)" in text
    assert "test_ft_cm33_firmware: fail (mtd1+0x1a0000 is blank: no CM33 image on the unit)" in text


def test_a_private_overlay_can_make_a_check_blocking_or_informational(tmp_path):
    over = tmp_path / "expect.yaml"
    over.write_text("informational: [eth_phy_id]\ngd32_protocol: '0.13.0'\ni2c_ids: {tps628640: {values: {0x44: 0x82}}}\n",
                    encoding="utf-8")
    x = functest.load_expect(over)
    assert x["i2c_ids"]["tps628640"] == {"reg": 1, "read": 1, "values": {0x48: 0x5A, 0x44: 0x82}}   # merged
    assert x["eth_phy_id"] == 0x001CC916                                                          # untouched
    ctx, _ = _setup(tmp_path, {"eth_phy_id": BAD["eth_phy_id"], "rtc_backup_mode": BAD["rtc_backup_mode"],
                               "gd32_bridge": BAD["gd32_bridge"], "i2c_tps628640_44": "0x80"}, functest_expect=x)
    res = _run(ctx)
    assert _val(res, "gd32_bridge") == "pass (protocol 0.13.0)"
    assert _val(res, "i2c_tps628640_44") == "fail (ID 0x80, want 0x82)"
    assert res.evidence["test_functional"] == "fail (rtc_backup_mode, i2c_tps628640_44)"   # eth_phy_id is informational now


def test_expect_file_schema_is_checked(tmp_path):
    bad = tmp_path / "e.yaml"
    bad.write_text("schema: 2\n", encoding="utf-8")
    with pytest.raises(ValueError, match="schema 1"):
        functest.load_expect(bad)


@pytest.mark.parametrize("value, reason", [
    ("pass", None),
    ("fail (eth_phy_id)", "test_functional: fail (eth_phy_id)"),
    ("unread (ssh: connect timed out)", "test_functional: unread (ssh: connect timed out)"),
    ("skipped (operator)", "test_functional: skipped (operator)"),
    ("", "missing test_functional (functional_test has not run on this unit)"),
    (None, "missing test_functional (functional_test has not run on this unit)")])
def test_ship_check_ships_only_a_passed_functional_test(value, reason):
    cat = {"disposition": {"group": "d", "source": "", "mode": "manual", "ship_required": True}}
    unit = {"disposition": "ship", "test_ft_wifi_scan": "skipped (no fixture: wifi_ap)",
            "test_ft_rtc_backup_mode": "fail (informational)"}
    if value is not None:
        unit["test_functional"] = value
    assert ledger_out.ship_check(unit, cat) == ([reason] if reason else [])


# --- plan, step order, time ---------------------------------------------------------------------

def test_step_order_runs_it_on_the_unit_as_shipped():
    n = steps.STEP_NAMES
    assert n.index("cold_boot_test") < n.index("census_final") < n.index("functional_test") < n.index("record")
    assert n.index("functional_test") < n.index("hil_smoke")


def test_dry_run_lists_every_check_and_touches_nothing(tmp_path):
    ctx, unit = _setup(tmp_path, fixtures={}, execute=False)
    res = steps.run_steps(ctx, only=["functional_test"])
    ft = res[-1]
    assert ft.status == "planned" and "would run 72 functional checks, estimated" in ft.detail
    assert unit.commands == [] and not unit.files
    assert any(c.startswith("WOULD: eth_phy_id: both PHYs answer on MDIO") for c in ft.commands)
    assert any("[skipped: no fixture wifi_ap]" in c for c in ft.commands)


def test_estimated_time_budget_is_far_under_90_s(tmp_path):
    ctx, _ = _setup(tmp_path, fixtures={})
    auto = functest.applicable(functest.build(ctx), {})
    lanes, wall = functest.estimate(auto)
    assert wall < 15 and max(lanes, key=lanes.get) == "main"
    ctx, _ = _setup(tmp_path / "all")
    _lanes, wall_all = functest.estimate(functest.applicable(functest.build(ctx), ALL_FIXTURES))
    assert wall_all < 30
    assert all(c.timeout_s >= 2 * c.est_s for c in functest.build(ctx))      # a timeout is a bound, not the estimate


def test_a_bad_bench_config_fails_the_step_with_a_clear_message(tmp_path):
    ctx, _ = _setup(tmp_path, carrier="no-such-board")
    res = steps.run_steps(ctx, only=["functional_test"])
    assert res[-1].status == "failed" and "no metadata/boards/no-such-board.yaml" in res[-1].detail


# --- the generated script: safe and well-formed ----------------------------------------------------

def _checks(tmp_path, **kw):
    ctx, _ = _setup(tmp_path, **kw)
    return functest.applicable(functest.build(ctx), ctx.bench.raw["functional_test"]["fixtures"])


def _script(tmp_path, **kw):
    return functest.script(_checks(tmp_path, **kw))


def test_the_script_writes_nothing_it_must_not(tmp_path):
    checks = _checks(tmp_path)
    body = "\n".join(functest.fragment(c) for c in checks)          # every command that runs on the unit
    for forbidden in (r"\bi2cset\b", r"\bflash_erase\b", r"mtd_debug\s+write", r"\bmmc\s+(bootpart|bootbus|write)",
                      r"\bunbind\b", r"/bind\b", r"\bof=/dev/(?!null)", r"hwclock", r"\bdd\b[^\n|;]*\bof=(?!/dev/null)",
                      r"force_ro", r"\bmkfs", r"\bfsck", r"/sys/class/gpio/export", r"bluetoothctl", r"/var/lib"):
        assert not re.search(forbidden, body), forbidden
    # every I2C write message is a register pointer (1 byte), the EEPROM / identity 2-byte
    # pointer, or the bridge's read-only GET_VERSION frame; nothing carries data
    for m in re.finditer(r"i2ctransfer (?:-f )?-y \S+ (w(\d+)@(0x[0-9a-f]{2})[^\n;|]*)", body):
        n, addr, msg = int(m[2]), int(m[3], 16), m[1]
        assert " r" in msg or msg.startswith("w1@0x30 0x82"), msg
        assert n == 1 or (n == 2 and addr in (0x50, 0x58)) or (n == 4 and addr == 0x70), msg
    assert re.findall(r"@0x58[^\n;|]*", body) == ["@0x58 0x00 0x00 r64 2>&1"]     # only the sealed secure-page read
    assert "0x58 0x04" not in body and "0x58 0x06" not in body
    # every redirect into a file stays inside the script's own temp directory (or /dev/null,
    # or the page-cache knob the write steps' readbacks also use)
    for target in re.findall(r"(?<![0-9&])>{1,2}\s*([^\s&;|)]+)", body):
        assert target.startswith(('"$D/', "/dev/null", "/proc/sys/vm/drop_caches")), target


def test_state_changing_checks_restore_in_a_trap_that_also_runs_when_killed(tmp_path):
    by = {c.name: c for c in _checks(tmp_path)}
    assert by["wifi_scan"].restore.endswith("ip link set wlan0 down")
    assert by["bt_hci"].restore.endswith("hciconfig hci0 down") and by["bt_scan"].restore.endswith("hciconfig hci0 down")
    assert "kill" in by["bt_scan"].restore                              # the running scan itself
    # CAN: back to the previous state, not just "down"
    assert 'bitrate "$b"' in by["can_loopback"].restore and "ip link set $c up" in by["can_loopback"].restore
    assert "f_$c=" in by["can_loopback"].setup and "b_$c=" in by["can_loopback"].setup  # ... which it recorded first
    for name in ("wifi_scan", "bt_hci", "bt_scan", "can_loopback"):
        frag = functest.fragment(by[name])
        assert "trap restore EXIT" in frag and "exit 143' TERM INT HUP" in frag
        # the state is recorded, the trap armed, and only then anything changes; the commands run
        # in the background so the waiting shell takes the TERM at once
        assert frag.index(by[name].setup) < frag.index("trap restore EXIT") < frag.index(by[name].cmd)
        assert frag.rstrip().endswith('body=$!\nwait "$body"\nexit $?')
        assert "down" not in by[name].cmd.replace("ip link set $c down 2>/dev/null; ip link set $c type can", "")


def test_the_emmc_read_drops_the_page_cache_first_with_the_readbacks_own_command(tmp_path):
    cmd = {c.name: c for c in _checks(tmp_path)}["emmc_read"].cmd
    assert cmd.startswith(lt.DROP_CACHES + "\n") and lt.DROP_CACHES == "sync; echo 3 > /proc/sys/vm/drop_caches"


def test_the_script_works_in_its_own_temp_dir_and_removes_it(tmp_path):
    lines = _script(tmp_path).splitlines()
    assert lines[2] == "D=$(mktemp -d /tmp/alp-ft.XXXXXX) || exit 1"
    assert lines[3] == "trap 'rm -rf \"$D\"' EXIT" and lines[4] == "trap 'exit 1' INT TERM HUP"
    assert lines[5] == 'cd "$D" || exit 1'                               # before any tool runs


def test_every_tool_a_fragment_uses_is_guarded(tmp_path):
    guarded = ("mmc", "ping", "iw", "hciconfig", "hcitool", "i2cget", "i2ctransfer", "dxrt-cli", "systemctl",
               "run_model", "aplay", "python3", "dmesg", "md5sum", "dd", "ip", "mountpoint", "seq", "awk")
    for c in _checks(tmp_path):
        used = {t for t in guarded if re.search(rf"(?:^|[\s;|(&$]){re.escape(t)}\s", c.cmd, re.M)}
        assert used <= set(c.tools), (c.name, used - set(c.tools))
        if c.tools:
            assert f"for t in {' '.join(c.tools)}; do command -v" in functest.fragment(c)


@pytest.mark.skipif(shutil.which("sh") is None, reason="no POSIX sh on this host")
def test_the_script_parses_as_posix_sh(tmp_path):
    p = tmp_path / "s.sh"
    p.write_bytes(_script(tmp_path).encode())
    r = subprocess.run(["sh", "-n", str(p)], capture_output=True, text=True, check=False)
    assert r.returncode == 0, r.stderr


HOSTILE = "a b; touch PWNED'\"$(touch PWNED2)"


@pytest.mark.skipif(shutil.which("sh") is None, reason="no POSIX sh on this host")
def test_values_from_bench_yaml_and_the_expect_file_are_quoted(tmp_path):
    """A space, a semicolon, both quotes and a command substitution in every string that goes from
    bench.yaml or the expected values into the script: the script still parses and each value
    appears only as one shell word."""
    x = functest.load_expect()
    x["platform_devices"] = {k: HOSTILE for k in ("drpai", "gpu", "sd")}
    x["gpu_device_node"] = HOSTILE
    x["carriers"]["e1m-x-evk"]["audio_card"] = HOSTILE
    fixtures = {**ALL_FIXTURES, "dxm1_model": HOSTILE, "uart_loopback": HOSTILE}
    ctx, _ = _setup(tmp_path, fixtures=fixtures, functest_expect=x)
    checks = functest.applicable(functest.build(ctx, x), fixtures)
    by = {c.name: c for c in checks}
    names = ("dxm1_inference", "uart_loopback", "audio", "drpai", "gpu", "sd_host")
    for name in names:
        cmd = by[name].cmd
        assert HOSTILE not in cmd.replace(shlex.quote(HOSTILE), ""), name      # only ever in its quoted form
        assert shlex.quote(HOSTILE) in cmd or shlex.quote("/sys/bus/platform/devices/" + HOSTILE + "/driver") in cmd
    p = tmp_path / "s.sh"
    p.write_bytes(functest.script(checks).encode())
    assert subprocess.run(["sh", "-n", str(p)], capture_output=True, text=True, check=False).returncode == 0
    # and run them for real (each stops at its "not there" test): the payload must not execute
    for name in names:
        r = subprocess.run(["sh", "-c", by[name].cmd], capture_output=True, text=True, check=False, cwd=tmp_path,
                           env={"D": tmp_path.as_posix(), "PATH": __import__("os").environ["PATH"]})
        assert not list(tmp_path.glob("PWNED*")), (name, r.stdout, r.stderr)


def test_a_pmic_expect_device_name_cannot_carry_shell(tmp_path):
    ctx, _ = _setup(tmp_path)
    ctx.expected_registers = {"devices": {"act; reboot": {"bus": "pmic", "addr": 0x25, "registers": [
        {"reg": 0x10, "expect": 0x08}]}}}
    with pytest.raises(ValueError, match="device name"):
        functest.build(ctx)


@pytest.mark.skipif(shutil.which("sh") is None, reason="no POSIX sh on this host")
def test_the_runner_frames_each_check_runs_lanes_concurrently_and_kills_an_overrun(tmp_path):
    """The shell runner itself, with harmless commands on the host's own sh: framing, one output
    file per check (no interleaving), lanes in parallel, the timeout: the hanging check's restore
    trap runs, and what it left running is killed with it."""
    C = functest.Check
    mark = (tmp_path / "restored").as_posix()
    late = (tmp_path / "late").as_posix()
    checks = [C("a", "", "echo first; echo err >&2; pwd", lambda o: None),
              C("slow1", "", "sleep 1; echo s1", lambda o: None, lane="x"),
              C("slow2", "", "sleep 1; echo s2", lambda o: None, lane="y"),
              C("hang", "", f"( sleep 6; echo late > {late} ) &\necho started; sleep 30", lambda o: None,
                timeout_s=1, lane="z", setup="state=was-down", restore=f"echo restored $state > {mark}"),
              C("rc", "", "echo before; exit 3", lambda o: None),
              C("nope", "", "definitely-not-a-command-xyz", lambda o: None),
              C("tool", "", "echo never", lambda o: None, tools=("sh", "definitely-not-a-tool-xyz"))]
    p = tmp_path / "s.sh"
    p.write_bytes(functest.script(checks).replace("/tmp/alp-ft.", (tmp_path / "ft.").as_posix()).encode())
    t0 = time.monotonic()
    r = subprocess.run(["sh", str(p)], capture_output=True, text=True, check=False, timeout=60)
    took = time.monotonic() - t0
    got = functest.parse(r.stdout)
    assert list(got) == ["a", "slow1", "slow2", "hang", "rc", "nope", "tool"]    # catalogue order, not finish order
    rc_a, out_a = got["a"]
    assert rc_a == "0" and out_a.splitlines()[:2] == ["first", "err"]
    assert "/ft." in out_a.splitlines()[2].replace("\\", "/")                 # the check ran inside the temp dir
    assert got["slow1"] == ("0", "s1") and got["slow2"] == ("0", "s2")
    assert got["hang"] == ("T", "started") and got["rc"] == ("3", "before") and got["nope"][0] == "127"
    assert got["tool"] == ("0", "ALPUNREAD missing tool: definitely-not-a-tool-xyz")
    assert took < 15, took                                                    # 1 s lanes in parallel, the hang killed at 1 s
    assert functest.judge(checks[3], got["hang"]) == "unread (timed out after 1 s)"
    assert functest.judge(checks[5], got["nope"]).startswith("unread (missing tool: ")
    assert functest.judge(checks[6], got["tool"]) == "unread (missing tool: definitely-not-a-tool-xyz)"
    # the restore trap ran on the kill, with the state its setup recorded
    assert Path(mark).read_text().strip() == "restored was-down"
    assert not list(tmp_path.glob("ft.*"))                                    # the work directory is removed
    if shutil.which("setsid"):
        time.sleep(7)                                                         # past the child's own 6 s
        assert not Path(late).exists()                                        # the whole process group was killed


def test_a_missing_tool_is_told_from_the_exit_status_or_the_last_line_only():
    c = functest.Check("x", "", "true", lambda o: "judged")
    assert functest.judge(c, ("127", "")) == "unread (missing tool: exit status 127)"
    assert functest.judge(c, ("1", "sh: iw: not found")) == "unread (missing tool: sh: iw: not found)"
    # a "not found" in the middle of a tool's own output (a kernel log, say) is judged normally
    assert functest.judge(c, ("0", "firmware: blob: not found\nall good")) == "pass (judged)"


def test_a_judge_that_meets_an_unexpected_shape_is_unread_not_a_crash():
    c = functest.Check("x", "", "true", lambda o: str(int(o)))
    assert functest.judge(c, ("0", "sh: arithmetic")).startswith("unread (unparsable output (ValueError: ")


# --- cold_boot_test sets the RTC only with the fixture ----------------------------------------------

def test_rtc_set_writes_the_time_registers_through_rtc0_only():
    t = FakeLinux({r"^python3 -c .*0x4024700a.* \d{10}$": "", r"^cat /proc/sys/kernel/random/boot_id$": "boot-a\n"})
    assert functest.rtc_set(t) == "boot-a"
    assert "/dev/rtc0" in t.commands[0] and "i2c" not in t.commands[0]


def _cold_boot_ctx(tmp_path, monkeypatch, fixtures, cycles):
    from provision import gates

    from .test_provision_steps import Board, _everyone_acks
    board = Board(act_0x10=0x08)
    board.rtc_sets, boots, orig = [], [], board._answer

    def answer(cmd):
        if cmd.startswith("i2cdetect"):
            return 0, _everyone_acks()
        if cmd.startswith("python3 -c") and "0x4024700a" in cmd:
            board.rtc_sets.append(len(boots))                      # after which cold cycle it was set
            return 0, ""
        if cmd == "cat /proc/sys/kernel/random/boot_id":
            return 0, f"boot-{len(boots)}\n"
        return orig(cmd)
    board._answer = answer
    text = f"NOTICE:  BL2: v2.10\nNOTICE:  BL2: SYS_LSI_MODE: 0X3c06\nDRAM:  3.9 GiB\n{gates.RAIL_PG}\n"
    monkeypatch.setattr(steps.ColdBootTest, "_cold_boot", staticmethod(lambda ctx, ev: boots.append(1) or text))
    bench = _bench()
    bench.raw = {"functional_test": {"fixtures": fixtures}}
    ctx = _ctx(tmp_path, bench=bench, linux=board, execute=True, cold_cycles=cycles)
    return ctx, board


def test_cold_boot_test_sets_the_rtc_after_its_first_cycle_when_the_fixture_is_on(tmp_path, monkeypatch):
    ctx, board = _cold_boot_ctx(tmp_path, monkeypatch, {"rtc_backup": True}, 3)
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    assert res[-1].status == "done", res[-1].detail
    assert board.rtc_sets == [1]                                    # once, with two real power cuts still to come
    assert res[-1].evidence["rtc_set_boot_id"] == "boot-1" == ctx.facts["rtc_set_boot_id"]


@pytest.mark.parametrize("fixtures, cycles", [({}, 3), ({"rtc_backup": False}, 3), ({"rtc_backup": True}, 1)])
def test_cold_boot_test_leaves_the_rtc_alone_otherwise(tmp_path, monkeypatch, fixtures, cycles):
    ctx, board = _cold_boot_ctx(tmp_path, monkeypatch, fixtures, cycles)
    res = steps.run_steps(ctx, only=["cold_boot_test"], force=["cold_boot_test"])
    assert res[-1].status == "done", res[-1].detail
    assert board.rtc_sets == [] and "rtc_set_boot_id" not in res[-1].evidence


# --- every criterion of the multi-criterion judges, one fault at a time ---------------------------

MORE_BAD = [
    ("boot_source", "emmc=\nroot=mmcblk1p2", "unread (no eMMC found"),
    ("emmc_health", "Life Time Estimation A [x]: 0x01\nLife Time Estimation B [x]: 0x02\nPre EOL information [x]: 0x01",
     "fail (life time A=0x01 B=0x02"),
    ("emmc_health", "Life Time Estimation A [x]: 0x01\nLife Time Estimation B [x]: 0x01\nPre EOL information [x]: 0x02",
     "fail (life time A=0x01 B=0x01, pre-EOL=0x02"),
    ("emmc_mode", "timing spec:\t9 (mmc HS200)\nhs200_failed=1", "fail (mmc_select_hs200 failed"),
    ("emmc_read", "12+0 records out\na=1.0 b=2.0", "fail (short or failed read"),
    ("emmc_read", "64+0 records out", "unread (no /proc/uptime stamps"),
    ("xspi", 'mtd0: 00060000 00001000 "bl2"\nmtd1: 00000000 00001000 "fip"\njedec=1122aa', "fail (mtd0/mtd1 not both"),
    ("xspi", 'mtd0: 00060000 00001000 "bl2"\nmtd1: 01fa0000 00001000 "fip"\njedec=000000', "fail (JEDEC ID"),
    ("eth_phy_id", "end0 0x00000000\nend1 0x001cc916", "fail (PHY ID {'end0': '0x00000000'}"),
    ("eth_mac", "end0 a2:c0:a6:00:00:10\nend1 a2:c0:a6:00:00:10", "fail (MACs"),
    ("eth0_link", "if=end0 carrier=0 speed=100 duplex=full\nping=ok\nrx_delta=4", "fail (end0: no carrier"),
    ("eth0_link", "if=end0 carrier=1 speed=10 duplex=full\nping=ok\nrx_delta=4", "fail (end0: link speed 10"),
    ("eth0_link", "if=end0 carrier=1 speed=100 duplex=half\nping=ok\nrx_delta=4", "fail (end0: duplex half"),
    ("eth0_link", "if=end0 carrier=1 speed=100 duplex=full\nping=fail\nrx_delta=4", "fail (end0: no ping reply"),
    ("eth0_link", "if=end0 carrier=1 speed=100 duplex=full\nping=ok\nrx_delta=0", "fail (end0: no ping reply"),
    ("wifi_present", "wlan0=present\nsdio=\n[ 7.9] brcmfmac: Firmware: BCM55500/1 wl0", "fail (no SDIO function"),
    ("wifi_present", "wlan0=present\nsdio=mmc2:0001:1\nFWBAD [ 7.0] brcmfmac: firmware load failed\n"
                     "[ 7.9] brcmfmac: Firmware: BCM55500/1", "fail (firmware load failure"),
    ("wifi_present", "wlan0=present\nsdio=mmc2:0001:1", "fail (firmware banner"),
    ("wifi_regdomain", "global", "unread (no country line"),
    ("wifi_scan", "command failed: Device or resource busy (-16)", "fail (the scan saw no network"),
    ("bt_hci", "", "fail (no hci0"),
    ("bt_hci", "HIL_BT_HCI_OK\nHIL_BT_UP_OK\n\tBD Address: 00:00:00:00:00:00  ACL MTU", "fail (hci0 has no BD address"),
    ("bt_scan", "LE Scan ...", "fail (the scan saw no advertiser"),
    ("bt_scan", "Set scan parameters failed: Input/output error", "unread (the scan did not run"),
    ("bt_scan", "ALPUNREAD missing tool: hcitool", "unread (missing tool: hcitool"),
    ("bt_hci", "ALPUNREAD missing tool: hciconfig", "unread (missing tool: hciconfig"),
    ("bt_hci", "HIL_BT_HCI_OK", "fail (hci0 does not come UP"),
    ("eth0_link", "ALPUNREAD missing tool: ping", "unread (missing tool: ping"),
    ("dxm1_pcie", "present=1 device=0x0000 width= speed=\ndriver=dx_dma_pcie", "unread (current_link_width"),
    ("dxm1_pcie", "present=1 device=0x0000 width=2 speed=5.0_GT/s_PCIe\ndriver=dx_dma_pcie", "fail (PCIe link speed '5.0 GT/s PCIe'"),
    ("systemd_failed", "ALPUNREAD systemctl rc=1: Failed to connect to bus: No such file or directory",
     "unread (systemctl rc=1: Failed to connect to bus"),
    ("systemd_failed", "Failed to connect to bus: No such file or directory", "unread (unexpected systemctl output"),
    ("emmc_health", "ALPUNREAD mmc extcsd read rc=1: open: No such file or directory", "unread (mmc extcsd read rc=1"),
    ("dxm1_pcie", "present=0", "fail (no PCIe endpoint at 0000:01:00.0"),
    ("dxm1_pcie", "present=1 device=0x0000 width=2 speed=8.0_GT/s_PCIe\ndriver=none", "fail (bound driver"),
    ("dxm1_pcie", "present=1 device=0x0000 width=1 speed=8.0_GT/s_PCIe\ndriver=dx_dma_pcie", "fail (PCIe link width x1"),
    ("dxm1_runtime", "service=active\n * FW version          : v2.4.0", "fail (no /dev/dxrt0"),
    ("dxm1_runtime", "dev=ok\nservice=active\n * RT Driver version   : v1.8.0", "unread (dxrt-cli -s printed no firmware"),
    ("dxm1_inference", "ALPUNREAD model /usr/share/model.dxnn is not on the unit", "unread (model "),
    ("dxm1_inference", "done\nrc=0", "fail (run_model output"),
    ("drpai", "driver=bound", "fail (no /dev/drpai* device node"),
    ("gpu", "/dev/mali0", "fail (no driver bound"),
    ("rtc_ticks", "a=0x1a b=0x1c", "fail (seconds register not BCD"),
    ("rtc_ticks", "a=0x10 b=0x30", "fail (seconds went 0x10 -> 0x30"),
    ("rtc_backup_mode", "Error: Read failed", "unread (no register 0x37"),
    ("board_temp", "0xe7 0x00", "fail (-25 degC outside 10..85 degC"),
    ("gd32_bridge", "0x00 0x00 0x0e 0x00 0xcf 0xa8", "fail (GD32 GET_VERSION reply CRC mismatch"),
    ("gd32_gpiochip", "", "unread (no gpiochip gd32-bridge-gpio"),
    ("pmic_registers", "R act88760 0x25 0x40 0x80\nR act88760 0x25 0x10 0x88", "fail (act88760 reg 0x10=0x88, want 0x08"),
    ("thermal", "", "unread (no thermal zone reports"),
    ("thermal", "Z thermal_zone0 106000", "fail (thermal_zone0=106 degC outside 10..105 degC"),
    ("cm33_firmware", "d41d8cd98f00b204e9800998ecf8427e  -", "unread (mtd1 not readable"),
    ("usb_device", "dev=sda\n0+0 records out", "fail (sda: read failed"),
    ("systemd_failed", "a.service loaded failed failed A\nb.mount loaded failed failed B", "fail (failed units: a.service b.mount"),
    ("dmesg_fatal", "<3>[ 9.0] renesas-gbeth 15c30000.ethernet end0: Failed to reset the dma", "fail (kernel log: "),
    ("dmesg_fatal", "<2>[ 9.0] EXT4-fs error (device mmcblk0p2): ext4_lookup:1855", "fail (kernel log: "),
    ("eeprom_manifest", "Error: Read failed", "unread (no reply"),
    ("secure_page", "0xff 0xff", "unread (i2ctransfer returned 2 bytes, wanted 64"),
    ("carrier_bmi323_68", "0x43 0x00 0x00 0x00", "fail (ID 0x00, want 0x43"),       # the ID is after 2 dummy bytes
    ("carrier_ina236_40", "Error: Read failed", "unread (no reply"),
    ("rail_3v3", "0x41 0x27\n0x08 0xb0\n0x0f 0xa0", "fail (3.558 V outside 3.135..3.465 V"),
    ("rail_3v3", "0x51 0x27\n0x08 0x0f\n0x7f 0xff", "pass (3.301 V, 1.024 A)"),    # ADCRANGE=1: 0.625 uV LSB
    ("rail_3v3", "0x41 0x27\n0x08 0x0f\n0x80 0x00", "fail (4.096 A outside 0..4 A"),  # magnitude, whichever the sign
    ("audio", "card=absent", "fail (no ALSA card e1m-x-evk-tas2563"),
    ("audio", "bound=2\naplay_rc=1\nelapsed=0.01", "fail (aplay rc=1"),
    ("audio", "bound=2\naplay_rc=0\nelapsed=5.5", "fail (5.5 s for a 2 s playback outside 1.8..2.5"),
    ("can_loopback", "ALPUNREAD the image has no can0/can1", "unread (the image has no can0/can1"),
    ("uart_loopback", "ALPUNREAD no tty /dev/ttySC1", "unread (no tty /dev/ttySC1"),
]


@pytest.mark.parametrize("name, answer, want", MORE_BAD, ids=[f"{n}-{i}" for i, (n, _a, _w) in enumerate(MORE_BAD)])
def test_each_criterion_of_a_check_is_judged(tmp_path, name, answer, want):
    ctx, _ = _setup(tmp_path, {name: answer})
    got = _val(_run(ctx), name)
    assert got.startswith(want), got


# --- the paths a mutation sweep found untested ----------------------------------------------------

def test_missing_preset_or_soc_data_is_unread_not_a_pass(tmp_path):
    ctx, _ = _setup(tmp_path)
    ctx.preset = {**ctx.preset, "memory": {}, "silicon": "nobody:nothing:x"}
    res = _run(ctx)
    assert _val(res, "cpu_count") == "unread (no expected value: the SoC description's core count)"
    assert _val(res, "mem_total") == "unread (no expected value: preset memory.dram_mbit)"
    assert _val(res, "emmc_size") == "unread (no expected value: preset memory.flash_mbit)"
    assert res.status == "failed"


def test_no_wlan0_fails_even_when_the_sdio_function_and_firmware_are_there(tmp_path):
    ctx, unit = _setup(tmp_path)
    unit.answers["wifi_present"] = unit.answers["wifi_present"].replace("wlan0=present\n", "")
    assert _val(_run(ctx), "wifi_present") == "fail (no wlan0)"


def test_pmic_registers_without_a_pmic_expect_file_is_unread(tmp_path):
    ctx, unit = _setup(tmp_path)
    ctx.expected_registers = None
    unit.answers["pmic_registers"] = ""
    assert _val(_run(ctx), "pmic_registers") == "unread (no expected value: --pmic-expect)"


def test_an_empty_kernel_log_is_unread_for_both_dmesg_checks(tmp_path):
    ctx, _ = _setup(tmp_path, {"dmesg_fatal": ""})
    res = _run(ctx)
    assert _val(res, "dmesg_fatal") == "unread (empty kernel log)" == _val(res, "dmesg_clean")
    assert res.evidence["test_functional"] == "fail (dmesg_fatal)"       # dmesg_clean stays informational


def test_manifest_that_differs_from_the_committed_copy_or_has_a_bad_crc_fails(tmp_path):
    ctx, unit = _setup(tmp_path)
    good = steps._build_blob(ctx, "program_eeprom.py", "manifest.bin")
    flipped = bytearray(good)
    flipped[0x40] ^= 0x01                                           # a payload byte; SKU, serial and magic intact
    unit.answers["eeprom_manifest"] = _hx(flipped)
    assert _val(_run(ctx), "eeprom_manifest") == "fail (no valid manifest (magic or CRC))"
    ctx.unit_dir.mkdir(parents=True, exist_ok=True)
    (ctx.unit_dir / f"{SERIAL}.manifest.bin").write_bytes(good)
    assert _val(_run(ctx), "eeprom_manifest") == f"fail (the array differs from the committed {SERIAL}.manifest.bin)"


# --- review round: key namespace, the ship check, stale verdicts --------------------------------------

def test_per_check_keys_never_touch_an_operator_entered_test_key(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    unit.write_text("disposition: ship\ntest_dxm1_inference: pass (operator, yolov8 on the bench)\n"
                    "test_wifi_scan: pass (operator)\n", encoding="utf-8")
    ctx, _ = _setup(tmp_path, fixtures={}, ledger_root=root)
    steps.run_steps(ctx, only=["functional_test", "record"])
    text = unit.read_text(encoding="utf-8")
    assert "test_dxm1_inference: pass (operator, yolov8 on the bench)" in text      # untouched
    assert "test_wifi_scan: pass (operator)" in text
    assert "test_ft_dxm1_inference: skipped (no fixture: dxm1_model)" in text       # the tool's own key
    keys = [ln.split(":")[0] for ln in text.splitlines() if ln.startswith("test_")]
    assert all(k.startswith("test_ft_") or k in ("test_functional", "test_dxm1_inference", "test_wifi_scan")
               for k in keys), keys


def test_a_unit_on_which_the_functional_test_never_ran_cannot_ship(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    ctx, _ = _setup(tmp_path, ledger_root=root)
    res = steps.run_steps(ctx, only=["record"])
    assert "blocked: missing test_functional (functional_test has not run on this unit)" in res[-1].detail
    res = steps.run_steps(ctx, only=["hil_smoke", "record"], skip=["functional_test"])
    assert "missing test_functional" in res[-1].detail


def _fresh_ctx(tmp_path, root, name, **kw):
    """A new invocation of the tool on the same ledger: empty facts, the state from disk."""
    ctx, unit = _setup(tmp_path / name, ledger_root=root, **kw)
    ctx.state = steps.load_state(ctx.state_path)
    return ctx, unit


def test_record_does_not_keep_a_stale_pass_after_a_later_failed_run(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    ctx, _ = _fresh_ctx(tmp_path, root, "a")
    res = steps.run_steps(ctx, only=["functional_test", "record"])
    assert "ship check: SHIPPABLE" in res[-1].detail and "test_functional: pass" in unit.read_text(encoding="utf-8")
    # a later `--only functional_test` fails (record is not part of that run) ...
    ctx, _ = _fresh_ctx(tmp_path, root, "b", answers={"eth_phy_id": BAD["eth_phy_id"]})
    res = steps.run_steps(ctx, only=["functional_test"])
    assert _statuses(res)["functional_test"] == "failed"
    assert "test_functional: pass" in unit.read_text(encoding="utf-8")              # still the old verdict on disk
    # ... and a later `--only record` must write the latest verdict, not keep the pass
    ctx, _ = _fresh_ctx(tmp_path, root, "c")
    res = steps.run_steps(ctx, only=["record"])
    text = unit.read_text(encoding="utf-8")
    assert "test_functional: fail (eth_phy_id)" in text and "test_functional: pass" not in text
    assert "test_ft_eth_phy_id: fail (" in text
    assert "blocked: test_functional: fail (eth_phy_id)" in res[-1].detail


@pytest.mark.parametrize("entry, want", [
    ({"status": "running", "at": "t"}, "unread (functional_test running in its latest run)"),      # killed mid-step
    ({"status": "failed", "at": "t", "detail": "boom", "evidence": {}}, "unread (functional_test failed in its latest run)"),
])
def test_record_turns_a_crashed_or_evidence_less_run_into_unread(tmp_path, entry, want):
    root, unit = _shippable_ledger(tmp_path)
    unit.write_text("disposition: ship\ntest_functional: pass\n", encoding="utf-8")
    ctx, _ = _setup(tmp_path, ledger_root=root)
    ctx.state = {"steps": {"functional_test": entry}}
    res = steps.run_steps(ctx, only=["record"])
    assert f"test_functional: {want}" in unit.read_text(encoding="utf-8")
    assert f"blocked: test_functional: {want}" in res[-1].detail


def test_a_run_that_cannot_reach_the_unit_records_an_unread_verdict(tmp_path):
    root, unit = _shippable_ledger(tmp_path)
    unit.write_text("disposition: ship\ntest_functional: pass\n", encoding="utf-8")
    ctx, fake = _setup(tmp_path, ledger_root=root)
    real = fake.run

    def run(cmd, **kw):
        if cmd.startswith("sh "):
            raise BenchError("timed out after 320s: sh /tmp/alp-functest.sh")
        return real(cmd, **kw)
    fake.run = run
    res = steps.run_steps(ctx, only=["functional_test", "record"])
    ft = res[1]
    assert ft.status == "failed" and ft.evidence == {"test_functional": "unread (timed out after 320s: sh /tmp/alp-functest.sh)"}
    assert "test_functional: unread (timed out after 320s" in unit.read_text(encoding="utf-8")
    assert "blocked: test_functional: unread (" in res[-1].detail
    # no Linux target at all (Refused) is the same: a verdict, not silence
    ctx, _ = _setup(tmp_path / "b", ledger_root=tmp_path / "b" / "none")
    ctx.linux = None
    r = steps.FunctionalTest().run(ctx)
    assert r.status == "failed" and r.evidence["test_functional"].startswith("unread (no Linux target attached")


# --- census_final marks what it could not re-read --------------------------------------------------

def test_census_final_marks_a_key_it_could_not_re_read_instead_of_keeping_the_first_value(tmp_path):
    from .test_provision_steps import Board
    cat = {**CATALOGUE, "keys": {**CATALOGUE["keys"], **{
        k: {"group": "power", "source": "", "mode": "auto", "ship_required": True}
        for k in ("tps_present", "clkgen_5l35023b_regs", "act88760_gpio_regs")}}}
    root = tmp_path / "ledger-cf"
    (root / "schema").mkdir(parents=True)
    (root / "schema" / "v2n.keys.yaml").write_text(yaml.safe_dump(cat), encoding="utf-8")
    board = Board(act_0x10=0x08)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True, ledger_root=root)
    res = steps.run_steps(ctx, only=["census", "record"])
    first = next(r for r in res if r.name == "census").evidence
    assert first["tps_present"] == "none" and first["clkgen_5l35023b_regs"].startswith("no ack")
    unit = ctx.unit_dir / f"{SERIAL}.unit.yaml"
    assert "tps_present: none" in unit.read_text(encoding="utf-8")
    # the final boot: the bus scan fails, so the groups that scan drop their keys
    orig = board._answer
    board._answer = lambda cmd: (1, "") if cmd.startswith("i2cdetect") else orig(cmd)
    res = steps.run_steps(ctx, only=["census_final", "record"])
    final = next(r for r in res if r.name == "census_final")
    assert final.evidence["tps_present"] == "unread (not re-read by census_final)"
    assert final.evidence["clkgen_5l35023b_regs"] == "unread (not re-read by census_final)"
    assert final.evidence["act88760_gpio_regs"] == "0x10=0x08"                     # re-read fine: a real value
    assert "tps_present" in final.detail and "not re-read" in final.detail
    assert not [k for k in final.evidence if k.startswith("act88760_gpio4_")]        # cold_boot_test's keys
    text = unit.read_text(encoding="utf-8")
    assert "tps_present: unread (not re-read by census_final)" in text and "tps_present: none" not in text
    assert "missing tps_present" in res[-1].detail and "missing clkgen_5l35023b_regs" in res[-1].detail


# --- the shared address, the carrier bus, preflight, CM33 --------------------------------------------

def test_no_override_can_produce_a_transfer_to_the_amplifiers_shared_address(tmp_path):
    over = tmp_path / "expect.yaml"
    over.write_text("carriers: {e1m-x-evk: {not_fitted: [], rails: {0x48: {name: cam2, volts: [1.0, 3.0]}}}}\n",
                    encoding="utf-8")
    x = functest.load_expect(over)
    assert x["carriers"]["e1m-x-evk"]["not_fitted"] == [0x48]            # the union: an override cannot shrink it
    x["carriers"]["e1m-x-evk"]["not_fitted"] = []                        # ... and even a hand-built dict cannot
    ctx, _ = _setup(tmp_path, functest_expect=x)
    checks = functest.build(ctx, x)
    assert not [c.name for c in checks if c.name.endswith("_48") and c.name.startswith(("carrier_", "rail_"))]
    assert "rail_cam2" not in {c.name for c in checks}
    body = functest.script(functest.applicable(checks, ALL_FIXTURES))
    assert not re.search(r"-y 0 [^\n;|]*@0x48\b", body)
    assert re.search(r"-y 8 w1@0x48 ", body)                              # the SoM's own 0x48 on the other bus stays
    board = functest._carrier("e1m-x-evk")
    assert functest._shared_addresses(board, "E1M_X_I2C0") == {0x48: "tas2563"}     # from the chip manifest


def test_the_shared_address_guard_refuses_whatever_built_the_command(tmp_path, monkeypatch):
    """Belt and braces: if the per-device filter were ever bypassed, the final guard refuses."""
    ctx, _ = _setup(tmp_path)
    real = functest._add_id

    def add_all(out, x, name, what, bus, addr, spec, **kw):
        real(out, x, name, what, bus, addr, spec, **kw)
        if name == "carrier_ina236_40":
            real(out, x, "carrier_ina236_48", what, bus, 0x48, spec, **kw)
    monkeypatch.setattr(functest, "_add_id", add_all)
    with pytest.raises(ValueError, match="refusing an I2C transfer to 0x48 on bus 0"):
        functest.build(ctx)


def test_the_carrier_bus_comes_from_the_board_description_and_a_null_bus_is_refused(tmp_path):
    ctx, _ = _setup(tmp_path)
    ctx.bench.i2c_bus["eeprom"] = 3
    by = {c.name: c for c in functest.build(ctx)}
    assert by["carrier_bmp581_47"].cmd.startswith("i2ctransfer -f -y 3 w1@0x47 ")
    assert by["eeprom_manifest"].cmd.startswith("i2ctransfer -y 3 w2@0x50 ")
    assert "/sys/bus/i2c/devices/3-004d/driver" in by["audio"].cmd
    ctx.bench.i2c_bus["eeprom"] = None
    with pytest.raises(ValueError, match="i2c_bus.eeprom is null"):
        functest.build(ctx)


@pytest.mark.parametrize("raw, want", [
    ({"functional_test": {"carrier": "no-such-board"}}, "no metadata/boards/no-such-board.yaml"),
    ({"functional_test": {"fixtures": ["wifi_ap"]}}, "want a mapping"),
    ({"functional_test": "yes"}, "want a mapping"),
])
def test_preflight_refuses_a_bad_functional_test_config_before_anything_is_flashed(tmp_path, raw, want):
    ctx, _ = _setup(tmp_path, execute=False)
    ctx.bench.raw = raw
    res = steps.run_steps(ctx, only=["preflight"])
    assert res[0].status == "failed" and "functional_test_config" in res[0].detail and want in res[0].detail
    ctx.bench.raw = {"functional_test": {"carrier": "e1m-x-evk"}}
    res = steps.run_steps(ctx, only=["preflight"])
    assert "functional_test_config" not in res[0].detail


def _cm33_bundle(tmp_path):
    from .test_provision_steps import _bundle
    d, b = _bundle(tmp_path)
    image = bytes(range(256)) * 20 + b"tail"                              # 5124 B: one whole block + a remainder
    (d / "artifacts" / "cm33.bin").write_bytes(image)
    b["components"].append({"role": "cm33", "file": "artifacts/cm33.bin", "size_bytes": len(image),
                            "sha256": functest.hashlib.sha256(image).hexdigest(), "flash_target": "xspi:mtd1"})
    return (d, b), image


def test_cm33_firmware_is_blocking_with_an_md5_compare_once_the_bundle_carries_an_image(tmp_path):
    bundle, image = _cm33_bundle(tmp_path)
    md5 = functest.hashlib.md5(image).hexdigest()
    ctx, unit = _setup(tmp_path, {"cm33_firmware": f"{md5}  -"}, bundle=bundle)
    x = functest.load_expect()
    assert "cm33_firmware" in x["informational"]                           # listed, and overridden
    by = {c.name: c for c in functest.build(ctx)}
    assert by["cm33_firmware"].blocking and "cm33_firmware" not in functest.informational(list(by.values()), x)
    assert by["cm33_firmware"].cmd == ("{ dd if=/dev/mtd1 bs=4096 skip=416 count=1 2>/dev/null; "
                                       "dd if=/dev/mtd1 bs=1 skip=1708032 count=1028 2>/dev/null; } | md5sum")
    assert _val(_run(ctx), "cm33_firmware") == f"pass (md5 {md5})"
    unit.answers["cm33_firmware"] = "0123456789abcdef0123456789abcdef  -"
    res = _run(ctx)
    assert _val(res, "cm33_firmware").startswith("fail (mtd1+0x1a0000 md5 0123")
    assert res.status == "failed" and res.evidence["test_functional"] == "fail (cm33_firmware)"
