# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_amp_window.py and scripts/gen_amp_window.py."""
from __future__ import annotations

import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_amp_window as gate  # noqa: E402
import gen_amp_window as gen  # noqa: E402

FILES = [str(gen.SOC_JSON), str(gen.OUT), gate.LINUX_DTSI, *gate.BOARD_DTS, gate.MAGIC_HOME]


def _tree(tmp_path: Path) -> Path:
    for rel in FILES:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, tmp_path / rel)
    return tmp_path


def test_real_tree_is_clean():
    assert gate.find_problems(REPO) == []


def test_clean_copy_passes(tmp_path):
    assert gate.find_problems(_tree(tmp_path)) == []


def test_header_matches_metadata():
    c = gen.load_carveout()
    h = (REPO / gen.OUT).read_text(encoding="utf-8")
    assert f"ALP_AMP_BEACON_A55_ADDR {c['a55_base'] + c['rsctbl_size'] - 16:#x}u" in h


def test_moved_window_in_metadata_is_caught_everywhere(tmp_path):
    root = _tree(tmp_path)
    soc = root / gen.SOC_JSON
    soc.write_text(soc.read_text(encoding="utf-8").replace('"a55_base": 1332740096', '"a55_base": 1332744192'),
                   encoding="utf-8")
    msgs = "\n".join(gate.find_problems(root))
    assert "alp_amp_window.h: stale" in msgs
    assert "reserved-memory node" in msgs
    assert "rsctbl uio node" in msgs


def test_drifted_linux_dt_and_board_dts_are_caught(tmp_path):
    root = _tree(tmp_path)
    dtsi = root / gate.LINUX_DTSI
    dtsi.write_text(dtsi.read_text(encoding="utf-8").replace("0x0 0x900000", "0x0 0x800000"), encoding="utf-8")
    dts = root / gate.BOARD_DTS[0]
    dts.write_text(dts.read_text(encoding="utf-8").replace("<0x9f700000 0x900000>", "<0x9f700000 0x800000>"),
                   encoding="utf-8")
    msgs = gate.find_problems(root)
    assert any("e1m-v2n-som.dtsi: reserved-memory node" in m for m in msgs)
    assert any("openamp_shm reg" in m and "v2n101" in m for m in msgs)


def test_second_copy_of_the_magic_is_caught(tmp_path):
    root = _tree(tmp_path)
    extra = root / "src" / "dup.c"
    extra.parent.mkdir(exist_ok=True)
    extra.write_text("#define M 0xA10D0683u\n", encoding="utf-8")
    assert any("src/dup.c: beacon magic restated" in m for m in gate.find_problems(root))
