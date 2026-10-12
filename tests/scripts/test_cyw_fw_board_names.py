"""The Wi-Fi firmware recipe ships a board-named symlink for each board DT
compatible so brcmfmac's first (board-specific) firmware lookup succeeds.
The recipe's name list must match the DT root compatibles."""

from __future__ import annotations

import re
from pathlib import Path

META = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
RECIPE = META / "recipes-kernel" / "cyw-fmac-firmware" / "cyw-fmac-firmware_git.bb"
DTS_DIR = META / "recipes-kernel" / "linux" / "linux-renesas"


def _dts_boards() -> set[str]:
    """Root compatible of every board dts that a machine conf builds."""
    boards = set()
    for conf in (META / "conf" / "machine").glob("e1m-v2*.conf"):
        m = re.search(r'^KERNEL_DEVICETREE\s*=\s*"[^"]*?([^/" ]+)\.dtb"', conf.read_text(encoding="utf-8"), re.M)
        assert m, f"no KERNEL_DEVICETREE in {conf.name}"
        dts = DTS_DIR / f"{m.group(1)}.dts"
        c = re.search(r'^\s*compatible\s*=\s*"([^"]+)"', dts.read_text(encoding="utf-8"), re.M)
        assert c, f"no root compatible in {dts.name}"
        boards.add(c.group(1))
    return boards


def test_recipe_board_names_match_the_dts_root_compatibles():
    m = re.search(r'^CYW_BOARD_NAMES\s*=\s*"([^"]+)"', RECIPE.read_text(encoding="utf-8"), re.M)
    assert m, "CYW_BOARD_NAMES missing"
    boards = _dts_boards()
    assert boards and set(m.group(1).split()) == boards
