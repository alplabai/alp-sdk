# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_pad_voltage.py (tmp_path trees, no subprocess)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_pad_voltage as gate  # noqa: E402


def _tree(tmp: Path, board: str) -> Path:
    (tmp / "metadata/socs/renesas/rzv2n").mkdir(parents=True)
    (tmp / "metadata/socs/renesas/rzv2n/p.json").write_text(json.dumps(
        {"pad_tolerance": {"max_signal_v": 1.89, "non_33v_tolerant_pads": ["P9[0-2]"]}}))
    (tmp / "metadata/pinmux").mkdir()
    (tmp / "metadata/pinmux/x.yaml").write_text(
        "pads:\n"
        "  - {e1m_pad: AD1, e1m_function: SPI0_MISO, owner: renesas, silicon_pad: P91}\n"
        "  - {e1m_pad: AD3, e1m_function: SPI1_MISO, owner: renesas, silicon_pad: P30}\n")
    (tmp / "metadata/boards").mkdir()
    (tmp / "metadata/boards/b.yaml").write_text(board)
    return tmp


HEAD = "name: B\nhosts_som_families: [renesas-rzv2n]\n"
SPI0 = "e1m_routes:\n  buses:\n    - {e1m: E1M_X_SPI0, macro: M}\n"


def test_clean_tree_passes(tmp_path):
    t = _tree(tmp_path, HEAD + SPI0 + "pad_levels:\n  - {pad: P91, signal_v: 1.8}\n")
    assert gate.find_problems(t) == []


def test_tolerant_pad_route_needs_nothing(tmp_path):
    t = _tree(tmp_path, HEAD + "e1m_routes:\n  buses:\n    - {e1m: E1M_X_SPI1, macro: M}\n")
    assert gate.find_problems(t) == []


def test_undeclared_route_fails(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + SPI0))
    assert len(p) == 1 and "P91" in p[0] and "not 3.3 V tolerant" in p[0]


def test_33v_without_shifter_fails_and_shifter_fixes(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + "pad_levels:\n  - {pad: P91, signal_v: 3.3}\n"))
    assert len(p) == 1 and "level_shifter" in p[0]
    (tmp_path / "metadata/boards/b.yaml").write_text(
        HEAD + "pad_levels:\n  - {pad: P91, signal_v: 3.3, level_shifter: translator}\n")
    assert gate.find_problems(tmp_path) == []


def test_board_hosting_another_soc_is_not_checked(tmp_path):
    board = ("name: B\nhosts_som_families: [alif-ensemble]\n" + SPI0
             + "pad_levels:\n  - {pad: P91, signal_v: 3.3}\n")
    assert gate.find_problems(_tree(tmp_path, board)) == []


def test_real_tree_clean():
    assert gate.find_problems(REPO) == []
