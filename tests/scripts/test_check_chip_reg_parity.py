# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_chip_reg_parity.py (issue #2347).

The gate hand-parses chips/act8760/act8760.c, chips/da9292/da9292.c and
chips/tps628640/tps628640.c against their metadata/chips/*.yaml manifests.
These tests copy the real corpus into a tmp_path tree and seed the four
mutations issue #2347 named, asserting the gate fires on each -- a green
run on the real repo alone proves nothing about whether the gate catches
drift.

Run locally:

    python3 -m pytest tests/scripts/test_check_chip_reg_parity.py -q
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_chip_reg_parity as gate  # noqa: E402

_RELPATHS = [
    "metadata/chips/act8760.yaml",
    "metadata/chips/da9292.yaml",
    "metadata/chips/tps628640.yaml",
    "chips/act8760/act8760.c",
    "chips/da9292/da9292.c",
    "chips/tps628640/tps628640.c",
    "include/alp/chips/tps628640.h",
]


def _copy_corpus(tmp_path: Path) -> Path:
    for rel in _RELPATHS:
        src = REPO / rel
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(src, dst)
    return tmp_path


def test_clean_tree_passes(tmp_path):
    _copy_corpus(tmp_path)
    assert gate.find_problems(tmp_path) == []


def test_act8760_rail_reg_typo_fails(tmp_path):
    root = _copy_corpus(tmp_path)
    p = root / "metadata" / "chips" / "act8760.yaml"
    p.write_text(p.read_text().replace("vset0_reg: 0x42,", "vset0_reg: 0x4A,"))
    problems = gate.find_problems(root)
    assert any("buck1" in msg and "vset0_reg" in msg for msg in problems)


def test_act8760_write_allow_flip_fails(tmp_path):
    root = _copy_corpus(tmp_path)
    p = root / "metadata" / "chips" / "act8760.yaml"
    p.write_text(
        p.read_text().replace(
            'name: "MODE4_MUX4", type: rw, write: guarded',
            'name: "MODE4_MUX4", type: rw, write: allow',
        )
    )
    problems = gate.find_problems(root)
    assert any("0x10" in msg for msg in problems)


def test_da9292_ch2_en_bit_move_fails(tmp_path):
    root = _copy_corpus(tmp_path)
    p = root / "chips" / "da9292" / "da9292.c"
    p.write_text(
        p.read_text().replace(
            "#define DA9292_CTRL01_CH2_EN     (1u << 1)",
            "#define DA9292_CTRL01_CH2_EN     (1u << 2)",
        )
    )
    problems = gate.find_problems(root)
    assert any("ch2" in msg and "en_bit" in msg for msg in problems)


def test_tps628640_vout_reg_typo_fails(tmp_path):
    root = _copy_corpus(tmp_path)
    p = root / "include" / "alp" / "chips" / "tps628640.h"
    p.write_text(
        p.read_text().replace(
            "#define TPS628640_REG_VOUT1   0x01u",
            "#define TPS628640_REG_VOUT1   0x04u",
        )
    )
    problems = gate.find_problems(root)
    assert any("vout1" in msg and "vout_reg" in msg for msg in problems)
