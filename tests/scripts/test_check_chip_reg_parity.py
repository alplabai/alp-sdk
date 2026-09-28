# SPDX-License-Identifier: Apache-2.0
"""scripts/check_chip_reg_parity.py (#2347): the hand-written ACT88760 /
DA9292 / TPS628640 register tables must match metadata/chips/<part>.yaml.

Each seeded mutation is one the issue names: a one-digit VSET0 slip, a
manifest row flipped to `write: allow` behind the driver's back, a moved
channel bit, a wrong VOUT base."""
from __future__ import annotations

import re
import shutil
from pathlib import Path

import pytest

import check_chip_reg_parity as g

REPO = Path(__file__).resolve().parents[2]
FILES = [
    "chips/act8760/act8760.c", "include/alp/chips/act8760.h",
    "chips/da9292/da9292.c",
    "include/alp/chips/tps628640.h",
    "metadata/chips/act8760.yaml", "metadata/chips/da9292.yaml", "metadata/chips/tps628640.yaml",
]


@pytest.fixture
def tree(tmp_path):
    for rel in FILES:
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, dst)
    return tmp_path


def _edit(tree: Path, rel: str, old: str, new: str) -> None:
    p = tree / rel
    s = p.read_text(encoding="utf-8")
    assert s.count(old) == 1, old
    p.write_text(s.replace(old, new), encoding="utf-8")


def test_real_tree_is_clean():
    assert g.find_problems(REPO) == []


def test_clean_copy_is_clean(tree):
    assert g.find_problems(tree) == []


def test_act8760_vset0_slip_is_caught(tree):
    # the issue's own example: Buck1 VSET0 0x42 mistyped as 0x4A in the manifest
    s = (tree / "metadata/chips/act8760.yaml").read_text(encoding="utf-8")
    s = re.sub(r"(id: buck1,.*?vset0_reg: )0x42", r"\g<1>0x4A", s, count=1)
    (tree / "metadata/chips/act8760.yaml").write_text(s, encoding="utf-8")
    errs = g.find_problems(tree)
    assert errs == ["act8760 rail buck1: vset0_reg metadata 0x4a != chips/act8760/act8760.c 0x42"]


def test_act8760_driver_table_slip_is_caught(tree):
    _edit(tree, "chips/act8760/act8760.c", "BUCK_F(0xC0u, 1u)", "BUCK_F(0xC8u, 1u)")
    errs = g.find_problems(tree)
    assert any(e.startswith("act8760 rail buck5: status_reg metadata 0xc0 != ") for e in errs), errs


def test_act8760_allow_flip_is_caught(tree):
    _edit(tree, "metadata/chips/act8760.yaml",
          'addr: 0x10, name: "MODE4_MUX4", type: rw, write: guarded',
          'addr: 0x10, name: "MODE4_MUX4", type: rw, write: allow')
    assert g.find_problems(tree) == [
        "act8760: 0x10 is `write: allow` in metadata but not in chips/act8760/act8760.c raw_write_allow[]"]


def test_da9292_channel_bit_is_caught(tree):
    _edit(tree, "chips/da9292/da9292.c", "#define DA9292_CTRL01_CH2_EN     (1u << 1)",
          "#define DA9292_CTRL01_CH2_EN     (1u << 0)")
    assert "da9292 ch2: en_bit metadata 0x01 != chips/da9292/da9292.c 0x00" in g.find_problems(tree)


def test_tps628640_vout_base_is_caught(tree):
    _edit(tree, "include/alp/chips/tps628640.h", "#define TPS628640_VOUT_BASE_MV 400u",
          "#define TPS628640_VOUT_BASE_MV 500u")
    errs = g.find_problems(tree)
    assert any(e.startswith("tps628640 vout1: base_mv metadata 0x190 != ") for e in errs), errs
