# SPDX-License-Identifier: Apache-2.0
"""The portable BOARD_* alias set must be identical across every board
routes header — a name present on only one board would let a "both"
example compile on one EVK and break on the other."""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROUTES = REPO / "include" / "alp" / "boards"
_BOARD_DEFINE = re.compile(r"^#define\s+(BOARD_[A-Z0-9_]+)\s", re.MULTILINE)


def _board_aliases(header: Path) -> set[str]:
    return set(_BOARD_DEFINE.findall(header.read_text(encoding="utf-8")))


def test_evk_and_x_evk_define_identical_board_alias_sets() -> None:
    evk = _board_aliases(ROUTES / "alp_e1m_evk_routes.h")
    xevk = _board_aliases(ROUTES / "alp_e1m_x_evk_routes.h")
    assert evk, "no BOARD_* aliases found in alp_e1m_evk_routes.h"
    assert xevk, "no BOARD_* aliases found in alp_e1m_x_evk_routes.h"
    assert evk == xevk, (
        "BOARD_* alias sets differ between EVKs:\n"
        f"  e1m-evk only:   {sorted(evk - xevk)}\n"
        f"  e1m-x-evk only: {sorted(xevk - evk)}"
    )


def test_board_dac0_is_present() -> None:
    """Pilot anchor: DAC0 is common (e1m-spec §7.2) so BOARD_DAC0 must exist."""
    assert "BOARD_DAC0" in _board_aliases(ROUTES / "alp_e1m_evk_routes.h")


BOARD_H = REPO / "include" / "alp" / "board.h"


def test_board_facade_selects_each_known_board() -> None:
    text = BOARD_H.read_text(encoding="utf-8")
    # Each known board's ALP_BOARD_<SLUG> must select its routes header.
    assert "ALP_BOARD_E1M_X_EVK" in text, "facade missing ALP_BOARD_E1M_X_EVK selector"
    assert "alp/boards/alp_e1m_x_evk_routes.h" in text, "facade missing x-evk routes include"
    assert "ALP_BOARD_E1M_EVK" in text, "facade missing ALP_BOARD_E1M_EVK selector"
    assert "alp/boards/alp_e1m_evk_routes.h" in text, "facade missing e1m-evk routes include"
    assert "#error" in text, "facade must hard-error when no board selected"
