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
    """Patch 0013's family -> dtb table must name the dtbs the images hold."""
    patch = (UBOOT / "0013-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch").read_text(encoding="utf-8")
    table = dict(re.findall(r'^\+	X\("([a-z0-9-]+)", "([^"]+\.dtb)"\)', patch, re.M))
    v2m = _machine_dtb(META / "conf" / "machine" / "e1m-v2m101-a55.conf")
    v2n = _machine_dtb(META / "conf" / "machine" / "e1m-v2n101-a55.conf")
    assert table == {"v2n-m1": v2m, "v2n": v2n}


def test_fdtfile_chain_has_one_alt_and_the_default_is_in_the_table():
    """bootcmd loads ${fdtfile} then ${fdtfile_alt}: that covers every distinct dtb
    only while the table has two rows and both build defaults are table dtbs."""
    patch = (UBOOT / "0013-rzv2n-dev-ALP-E1M-fdtfile-from-eeprom.patch").read_text(encoding="utf-8")
    table = re.findall(r'^\+	X\("[a-z0-9-]+", "([^"]+\.dtb)"\)', patch, re.M)
    v2m = re.search(r'CONFIG_ALP_E1M_FDTFILE="([^"]+)"', (UBOOT / "fdtfile-v2m.cfg").read_text(encoding="utf-8")).group(1)
    v2n = re.search(r'default "(e1m-v2n[^"]+\.dtb)"', (UBOOT / "0002-rzv2n-dev-ALP-E1M-production-boot.patch").read_text(encoding="utf-8")).group(1)
    assert len(table) == 2 and {v2m, v2n} <= set(table)
    assert "boot/${fdtfile_alt}; then" in patch and "ALP_FDTFILE_TRY" not in patch
