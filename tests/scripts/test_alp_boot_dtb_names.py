"""Each V2N/V2M machine's KERNEL_DEVICETREE basename must be the dtb U-Boot falls
back to (CONFIG_ALP_E1M_FDTFILE), so the image's vendor-name link points at the
dtb that really boots."""

from __future__ import annotations

import re
from pathlib import Path

META = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
UBOOT = META / "recipes-bsp" / "u-boot" / "u-boot"


def _machine_dtb(conf: Path) -> str:
    m = re.search(r'^KERNEL_DEVICETREE\s*=\s*"[^"]*?([^/" ]+\.dtb)"', conf.read_text(encoding="utf-8"), re.M)
    assert m, conf.name
    return m.group(1)


def test_machine_dtb_matches_the_u_boot_fdtfile():
    v2m = re.search(r'CONFIG_ALP_E1M_FDTFILE="([^"]+)"', (UBOOT / "fdtfile-v2m.cfg").read_text(encoding="utf-8")).group(1)
    patch = (UBOOT / "0002-rzv2n-dev-ALP-E1M-production-boot.patch").read_text(encoding="utf-8")
    v2n = re.search(r'default "(e1m-v2n[^"]+\.dtb)"', patch).group(1)
    confs = sorted((META / "conf" / "machine").glob("e1m-v2[mn]10?-a55.conf"))
    assert len(confs) == 6
    for conf in confs:
        expected = v2m if "v2m" in conf.name else v2n
        assert _machine_dtb(conf) == expected, conf.name


def test_alp_fdtfile_family_table_matches_the_machine_dtbs():
    """Patch 0014's family -> dtb table must name the dtbs the images hold."""
    patch = (UBOOT / "0014-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch").read_text(encoding="utf-8")
    table = dict(re.findall(r'^\+	X\("([a-z0-9-]+)", "([^"]+\.dtb)"\)', patch, re.M))
    v2m = _machine_dtb(META / "conf" / "machine" / "e1m-v2m101-a55.conf")
    v2n = _machine_dtb(META / "conf" / "machine" / "e1m-v2n101-a55.conf")
    assert table == {"v2n-m1": v2m, "v2n": v2n}


def test_fdtfile_chain_has_one_alt_and_the_default_is_in_the_table():
    """bootcmd loads ${fdtfile} then ${fdtfile_alt}: that covers every distinct dtb
    only while the table has two rows and both build defaults are table dtbs."""
    patch = (UBOOT / "0014-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch").read_text(encoding="utf-8")
    table = re.findall(r'^\+	X\("[a-z0-9-]+", "([^"]+\.dtb)"\)', patch, re.M)
    v2m = re.search(r'CONFIG_ALP_E1M_FDTFILE="([^"]+)"', (UBOOT / "fdtfile-v2m.cfg").read_text(encoding="utf-8")).group(1)
    v2n = re.search(r'default "(e1m-v2n[^"]+\.dtb)"', (UBOOT / "0002-rzv2n-dev-ALP-E1M-production-boot.patch").read_text(encoding="utf-8")).group(1)
    assert len(table) == 2 and {v2m, v2n} <= set(table)
    assert "boot/${fdtfile_alt}; then" in patch and "ALP_FDTFILE_TRY" not in patch


def test_fdtfile_chain_fails_closed_for_a_known_family():
    """A known family sets no fdtfile_alt and the bootcmd refuses on an empty one."""
    patch = (UBOOT / "0014-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch").read_text(encoding="utf-8")
    assert 'elif test -z \\"${fdtfile_alt}\\"; then' in patch
    assert "refusing to boot the device tree of another SoM" in patch
    assert "if (!known) {" in patch  # fdtfile_alt is computed only for the no-family case
    assert "CMD_RET_FAILURE" in patch


def test_no_single_quote_in_any_patched_bootcmd_line():
    """U-Boot's old hush parser treats ' as a quote even inside a double-quoted
    echo: an apostrophe in CONFIG_BOOTCOMMAND gives 'syntax error' and, on the
    autoboot path, a Synchronous Abort reset loop (bench, E1M-V2M103, #2694)."""
    for patch in sorted(UBOOT.glob("*.patch")):
        text = patch.read_text(encoding="utf-8")
        added = [l for l in text.splitlines() if l.startswith("+") and not l.startswith("+++")]
        in_bootcmd = False
        for line in added:
            if "CONFIG_BOOTCOMMAND" in line or "bootcmd=" in line:
                in_bootcmd = True
            if in_bootcmd:
                assert "'" not in line, f"{patch.name}: {line}"
                if not line.rstrip().endswith("\\"):
                    in_bootcmd = False


def test_gbeth_phy_fixup_patch_is_in_the_series_and_skips_broadcast_address():
    """Patch 0017 (#2582) scans MDIO 1..31, never the broadcast address 0, and
    must be wired into the bbappend."""
    name = "0017-rzv2n-dev-ALP-E1M-gbeth-phy-address-fixup.patch"
    bbappend = (META / "recipes-bsp" / "u-boot" / "u-boot_%.bbappend").read_text(encoding="utf-8")
    assert f"file://{name}" in bbappend
    patch = (UBOOT / name).read_text(encoding="utf-8")
    assert "for (addr = 1; addr < 32; addr++)" in patch
    assert "addr = 0;" not in patch
    assert "ALP_PHY_OUI_ID1		0x001c" in patch and "0xc916" in patch
    assert "PHY at MDIO addr %d (DT had %d) - fixed" in patch
    assert "&eth1" not in patch
    assert "CLKMON" not in patch and "RSTMON" not in patch
    assert "get_timer(" in patch and "-ETIMEDOUT" in patch
    scan = patch[patch.index("static int alp_scan_phy_addr"):]
    assert "if (ret)\n+\t\t\treturn ret;" in scan
