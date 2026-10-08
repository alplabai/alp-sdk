"""A board dts that several SKU machines share must not name one SKU in its
"model": an E1M-V2M103 printed "Machine model: ALP E1M-V2M101 ..." because the
V2M101/102/103 machines all build e1m-v2m101-x-evk.dtb. The unit's SKU comes
from U-Boot's /chosen/alp,sku instead."""

from __future__ import annotations

import re
from collections import defaultdict
from pathlib import Path

META = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
DTS_DIR = META / "recipes-kernel" / "linux" / "linux-renesas"


def test_shared_board_dts_model_names_no_sku():
    users = defaultdict(list)
    for conf in (META / "conf" / "machine").glob("e1m-v2*.conf"):
        m = re.search(r'^KERNEL_DEVICETREE\s*=\s*"[^"]*?([^/" ]+)\.dtb"', conf.read_text(encoding="utf-8"), re.M)
        assert m, f"no KERNEL_DEVICETREE in {conf.name}"
        users[m.group(1)].append(conf.name)
    shared = {dts: confs for dts, confs in users.items() if len(confs) > 1}
    assert shared, "expected the V2N/V2M SKU machines to share board dtbs"
    for dts, confs in shared.items():
        model = re.search(r'^\s*model\s*=\s*"([^"]+)"', (DTS_DIR / f"{dts}.dts").read_text(encoding="utf-8"), re.M)
        assert model, f"no model in {dts}.dts"
        assert not re.search(r"V2[MN]\d{3}", model.group(1)), f"{dts}.dts model names a SKU but serves {sorted(confs)}"
