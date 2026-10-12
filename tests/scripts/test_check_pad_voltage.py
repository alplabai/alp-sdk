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
    for vendor, fam, tol in (("acme", "x1", {"max_signal_v": 1.89, "non_33v_tolerant_pads": ["P9[0-2]", "P5"]}),
                             ("other", "y1", None)):
        d = tmp / f"metadata/socs/{vendor}/{fam}"
        d.mkdir(parents=True)
        (d / "p.json").write_text(json.dumps({"pad_tolerance": tol} if tol else {}), encoding="utf-8")
    (tmp / "metadata/e1m_modules").mkdir()
    (tmp / "metadata/e1m_modules/E1M-A1.yaml").write_text(
        "family: acme-x1\nsilicon: acme:x1:p\n", encoding="utf-8")
    (tmp / "metadata/e1m_modules/E1M-B1.yaml").write_text(
        "family: other-y1\nsilicon: other:y1:p\n", encoding="utf-8")
    (tmp / "metadata/pinmux").mkdir()
    (tmp / "metadata/pinmux/x.yaml").write_text(
        "som_families: [acme-x1]\n"
        "pads:\n"
        "  - {e1m_pad: AD1, e1m_function: SPI0_MISO, owner: acme, silicon_peripheral: a, silicon_pad: P91}\n"
        "  - {e1m_pad: AD3, e1m_function: SPI1_MISO, owner: acme, silicon_peripheral: b, silicon_pad: P30}\n"
        "  - {e1m_pad: TBD, e1m_function: IO7, owner: acme, silicon_peripheral: c, silicon_pad: P5}\n"
        "  - {e1m_pad: AD4, e1m_function: SPI0_MOSI, owner: sidechip, silicon_peripheral: d, silicon_pad: P90}\n", encoding="utf-8")
    (tmp / "metadata/pinmux/y.yaml").write_text(
        "som_families: [other-y1]\n"
        "pads:\n"
        "  - {e1m_pad: AD1, e1m_function: SPI0_MISO, owner: other, silicon_peripheral: a, silicon_pad: P91}\n", encoding="utf-8")
    (tmp / "metadata/boards").mkdir()
    (tmp / "metadata/boards/b.yaml").write_text(board, encoding="utf-8")
    return tmp


HEAD = "name: B\nhosts_som_families: [acme-x1]\n"
SPI0 = "e1m_routes:\n  buses:\n    - {e1m: E1M_X_SPI0, macro: M}\n"
IO7 = "e1m_routes:\n  gpio:\n    - {e1m: E1M_X_GPIO_IO7, macro: M}\n"


def test_clean_tree_passes(tmp_path):
    t = _tree(tmp_path, HEAD + SPI0 + "pad_levels:\n  - {e1m: E1M_X_SPI0, signal_v: 1.8}\n")
    assert gate.find_problems(t) == []


def test_tolerant_pad_route_needs_nothing(tmp_path):
    t = _tree(tmp_path, HEAD + "e1m_routes:\n  buses:\n    - {e1m: E1M_X_SPI1, macro: M}\n")
    assert gate.find_problems(t) == []


def test_undeclared_route_fails(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + SPI0))
    assert len(p) == 1 and "P91" in p[0] and "not 3.3 V tolerant" in p[0]


def test_gpio_route_is_resolved_even_without_an_e1m_pad(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + IO7))
    assert len(p) == 1 and "P5" in p[0]


def test_other_owner_rows_are_not_the_host_soc(tmp_path):
    # the `sidechip` row on P90 must not count as a host-SoC pad: only P91 is reported.
    p = gate.find_problems(_tree(tmp_path, HEAD + SPI0))
    assert "P90" not in p[0]


def test_33v_without_shifter_fails_and_shifter_fixes(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + SPI0 + "pad_levels:\n  - {e1m: E1M_X_SPI0, signal_v: 3.3}\n"))
    assert len(p) == 1 and "level_shifter" in p[0]
    (tmp_path / "metadata/boards/b.yaml").write_text(
        HEAD + SPI0 + "pad_levels:\n  - {e1m: E1M_X_SPI0, signal_v: 3.3, level_shifter: translator}\n", encoding="utf-8")
    assert gate.find_problems(tmp_path) == []


def test_pad_levels_for_undeclared_route_fails(tmp_path):
    p = gate.find_problems(_tree(tmp_path, HEAD + "pad_levels:\n  - {e1m: E1M_X_SPI0, signal_v: 1.8}\n"))
    assert len(p) == 1 and "does not declare" in p[0]


def test_board_hosting_a_soc_without_tolerance_is_not_checked(tmp_path):
    board = "name: B\nhosts_som_families: [other-y1]\n" + SPI0
    assert gate.find_problems(_tree(tmp_path, board)) == []


def test_each_hosted_som_is_checked_against_its_own_table(tmp_path):
    board = "name: B\nhosts_som_families: [acme-x1, other-y1]\n" + SPI0
    p = gate.find_problems(_tree(tmp_path, board))
    assert len(p) == 1 and "acme:x1:p" in p[0]


def test_real_tree_clean():
    assert gate.find_problems(REPO) == []
