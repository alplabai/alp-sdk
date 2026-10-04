"""Each V2N/V2M machine's KERNEL_DEVICETREE basename must be the dtb U-Boot loads
(CONFIG_ALP_E1M_FDTFILE), so the image's vendor-name link points at the dtb that
really boots."""

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
